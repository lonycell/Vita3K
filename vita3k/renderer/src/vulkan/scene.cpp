// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <renderer/vulkan/functions.h>

#include <gxm/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>

#include <config/state.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <util/log.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace renderer::vulkan {

// Guest address of the most recently bound vertex uniform block, per host block index
// (0 = default uniform buffer, 1.. = SceGxm uniform buffer index + 1). Used by the CPU
// vertex-job skinning path (try_cpu_vertex_job_skin) to locate the bone-matrix and output
// buffers, which the skinning shader would otherwise reach through memory-mapped device addresses.
static constexpr int MAX_VERTEX_UNIFORM_BLOCKS = 15;
static uint32_t g_vertex_uniform_block_addr[MAX_VERTEX_UNIFORM_BLOCKS] = {};

void set_uniform_buffer(VKContext &context, MemState &mem, const ShaderProgram *program, const bool vertex_shader, const int block_num, const int size, Ptr<uint8_t> data) {
    if (vertex_shader && block_num >= 0 && block_num < MAX_VERTEX_UNIFORM_BLOCKS)
        g_vertex_uniform_block_addr[block_num] = data.address();

    auto offset = program->uniform_buffer_data_offsets.at(block_num);
    if (offset == static_cast<std::uint32_t>(-1)) {
        return;
    }

    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);
    if (context.state.features.enable_memory_mapping) {
        if (context.state.mapping_method == MappingMethod::DoubleBuffer) {
            // we must always cover everything as some small part of the buffer may get changed only
            context.state.buffer_trapping.access_buffer(data.address(), data_size_upload, mem, false, true);
        }

        const uint64_t buffer_address = context.state.get_matching_device_address(data.address());
        if (vertex_shader) {
            context.curr_vert_ublock.set_buffer_address(block_num, buffer_address);
        } else {
            context.curr_frag_ublock.set_buffer_address(block_num, buffer_address);
        }
    } else {
        const uint32_t offset_start_upload = offset * 4;

        if (vertex_shader) {
            if (!context.vertex_uniform_storage_allocated) {
                // Allocate a region for it. Don't worry though, when the shader program is changed
                context.vertex_uniform_stream_ring_buffer.allocate(program->max_total_uniform_buffer_storage * 4);
                context.vertex_uniform_storage_allocated = true;
            }

            context.vertex_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, data.get(mem), offset_start_upload);
        } else {
            if (!context.fragment_uniform_storage_allocated) {
                // Allocate a region for it. Don't worry though, when the shader program is changed
                context.fragment_uniform_stream_ring_buffer.allocate(program->max_total_uniform_buffer_storage * 4);
                context.fragment_uniform_storage_allocated = true;
            }

            context.fragment_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, data.get(mem), offset_start_upload);
        }
    }
}

void mid_scene_flush(VKContext &context, const SceGxmNotification notification) {
    // two cases :
    // notification.addr is 0: this means that the mid scene flush must be used as a barrier in the renderpass
    // notification.addr is not 0: this means the app is waiting for this part to be finished to re-use the resources

    // Note: however, when testing, the barrier inside a pipeline does not work (or not entirely, depending on the GPU)
    // maybe because I'm writing using buffer device addresses, not sure...
    // so for the time being always restart the render pass
    // const bool restart_render_pass = notification.address.address() != 0;
    const bool restart_render_pass = true;

    if (restart_render_pass && context.in_renderpass)
        context.stop_render_pass();

    // in case there is no notification, this will happen in the render pass
    vk::MemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead,
    };
    context.render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader, vk::PipelineStageFlagBits::eVertexInput,
        vk::DependencyFlags(), barrier, {}, {});

    if (restart_render_pass) {
        SceGxmNotification empty_notification = { Ptr<uint32_t>(0), 0 };
        const bool submit = notification.address.address() != 0;
        context.stop_recording(notification, empty_notification, submit);
        context.start_recording();
        context.scene_timestamp++;
    }
}

#ifdef __APPLE__
// restride vertex attribute binding strides to multiple of 4
// needed for metal because it only allows multiples of 4.
void restride_stream(const uint8_t *&stream, uint32_t &size, uint32_t stride) {
    const uint32_t new_stride = align(stride, 4);
    const uint32_t nb_vertex_input = ((size + stride - 1) / stride);

    uint8_t *new_data = new uint8_t[nb_vertex_input * new_stride];
    for (uint32_t i = 0; i < nb_vertex_input; i++) {
        memcpy(new_data + new_stride * i, stream + stride * i, stride);
    }

    stream = new_data;
    size = nb_vertex_input * new_stride;
}
#endif

// when needed, how many descriptor of the given size we allocate for each frame at once
static constexpr uint32_t DESCRIPTOR_PACK_SIZE = 64;

static vk::DescriptorSet retrieve_descriptor(VKContext &context, bool is_vertex, uint16_t textures_count) {
    if (textures_count == 0)
        return context.empty_set;

    VKState &state = context.state;
    FrameDescriptor &frame_descriptor = is_vertex ? state.frame().vert_descriptors[textures_count - 1] : state.frame().frag_descriptors[textures_count - 1];
    if (frame_descriptor.descriptors_idx < frame_descriptor.sets.size())
        return frame_descriptor.sets[frame_descriptor.descriptors_idx++];

    // we have no more frame descriptor available, create a bunch of new one for this specific layout
    vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = textures_count * DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };

    vk::DescriptorPoolCreateInfo descriptor_pool_info{
        .maxSets = DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };
    descriptor_pool_info.setPoolSizes(pool_size);

    vk::DescriptorPool descriptor_pool = state.device.createDescriptorPool(descriptor_pool_info);
    state.frame_descriptor_pools.push_back(descriptor_pool);

    // allocate all the descriptor sets
    const vk::DescriptorSetLayout set_layout = is_vertex ? state.pipeline_cache.vertex_textures_layout[textures_count] : state.pipeline_cache.fragment_textures_layout[textures_count];
    std::vector<vk::DescriptorSetLayout> layouts(DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING, set_layout);
    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool
    };
    descr_set_info.setSetLayouts(layouts);
    auto descriptor_sets = state.device.allocateDescriptorSets(descr_set_info);

    // distribute them among all frames
    for (int frame_idx = 0; frame_idx < MAX_FRAMES_RENDERING; frame_idx++) {
        FrameObject &frame_object = state.frames[frame_idx];
        FrameDescriptor &frame_descr = is_vertex ? frame_object.vert_descriptors[textures_count - 1] : frame_object.frag_descriptors[textures_count - 1];

        // insert DESCRIPTOR_PACK_SIZE in each frame descriptor
        auto descr_it = descriptor_sets.begin() + frame_idx * DESCRIPTOR_PACK_SIZE;
        frame_descr.sets.insert(frame_descr.sets.end(), descr_it, descr_it + DESCRIPTOR_PACK_SIZE);
    }

    return frame_descriptor.sets[frame_descriptor.descriptors_idx++];
}

static void draw_bind_descriptors(VKContext &context, MemState &mem) {
    VKState &state = context.state;

    std::array<vk::DescriptorSet, 4> descriptors;
    descriptors[0] = context.global_set;
    descriptors[1] = context.rendertarget_set;

    const uint16_t vertex_textures_count = context.record.vertex_program.get(mem)->renderer_data->texture_count;
    const uint16_t fragment_texture_count = context.record.fragment_program.get(mem)->renderer_data->texture_count;

    vk::PipelineLayout pipeline_layout = state.pipeline_cache.pipeline_layouts[vertex_textures_count][fragment_texture_count];

    // try to use last descriptor if it still matches
    bool need_vert_descr = (vertex_textures_count != context.last_vert_texture_count);
    bool need_frag_descr = (fragment_texture_count != context.last_frag_texture_count);

    context.last_vert_texture_count = vertex_textures_count;
    context.last_frag_texture_count = fragment_texture_count;

    {
        if (need_vert_descr) {
            context.last_vert_texture_descriptor = retrieve_descriptor(context, true, vertex_textures_count);
        }
        descriptors[2] = context.last_vert_texture_descriptor;

        if (need_frag_descr) {
            context.last_frag_texture_descriptor = retrieve_descriptor(context, false, fragment_texture_count);
        }
        descriptors[3] = context.last_frag_texture_descriptor;
    }

    // bind textures
    std::array<vk::WriteDescriptorSet, 16> write_descrs;
    // some default sampler in case a slot has never been set and we read a slot with higher idx
    vk::DescriptorImageInfo default_image_info{
        .sampler = context.state.default_image.sampler,
        .imageView = context.state.default_image.view,
        .imageLayout = vk::ImageLayout::eGeneral
    };

    // vertex
    if (need_vert_descr) {
        for (uint32_t i = 0; i < vertex_textures_count; i++) {
            write_descrs[i] = vk::WriteDescriptorSet{
                .dstSet = descriptors[2],
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            };
            write_descrs[i].setImageInfo(context.vertex_textures[i].sampler ? context.vertex_textures[i] : default_image_info);
        }
        state.device.updateDescriptorSets(vertex_textures_count, write_descrs.data(), 0, nullptr);
    }

    // fragment
    if (need_frag_descr) {
        for (uint32_t i = 0; i < fragment_texture_count; i++) {
            write_descrs[i] = vk::WriteDescriptorSet{
                .dstSet = descriptors[3],
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            };
            write_descrs[i].setImageInfo(context.fragment_textures[i].sampler ? context.fragment_textures[i] : default_image_info);
        }
        state.device.updateDescriptorSets(fragment_texture_count, write_descrs.data(), 0, nullptr);
    }

    const uint32_t dynamic_offset_count = state.features.enable_memory_mapping ? 2U : 4U;
    const uint32_t dynamic_offsets[] = {
        // GXMRenderVertUniformBlock
        context.vertex_info_uniform_buffer.data_offset,
        // GXMRenderFragUniformBlock
        context.fragment_info_uniform_buffer.data_offset,
        // vertex ssbo
        context.vertex_uniform_stream_ring_buffer.data_offset,
        // fragment ssbo
        context.fragment_uniform_stream_ring_buffer.data_offset
    };

    context.render_cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
        descriptors.size(), descriptors.data(), dynamic_offset_count, dynamic_offsets);
}

// vertex count is only used with double buffer mapping
static void bind_vertex_streams(VKContext &context, MemState &mem, uint32_t instance_count, uint32_t max_index) {
    GxmRecordState &state = context.record;
    const SceGxmVertexProgram &vertex_program = *state.vertex_program.get(mem);
    VertexProgram *vkvert = vertex_program.renderer_data.get();

    int max_stream_idx = -1;

    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (!vkvert->attribute_infos.contains(attribute.regIndex))
            continue;
        max_stream_idx = std::max<int>(max_stream_idx, attribute.streamIndex);
    }
    max_stream_idx++;

    if (context.state.mapping_method == MappingMethod::DoubleBuffer) {
        for (int i = 0; i < max_stream_idx; i++)
            state.vertex_streams[i].size = 0;

        // same as in SceGxm.cpp
        for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
            const SceGxmAttributeFormat attribute_format = static_cast<SceGxmAttributeFormat>(attribute.format);
            const size_t attribute_size = gxm::attribute_format_size(attribute_format) * attribute.componentCount;
            const SceGxmVertexStream &stream = vertex_program.streams[attribute.streamIndex];
            const SceGxmIndexSource index_source = static_cast<SceGxmIndexSource>(stream.indexSource);
            const size_t data_passed_length = gxm::is_stream_instancing(index_source) ? ((instance_count - 1) * stream.stride) : (max_index * stream.stride);
            const size_t data_length = attribute.offset + data_passed_length + attribute_size;
            state.vertex_streams[attribute.streamIndex].size = std::max<size_t>(state.vertex_streams[attribute.streamIndex].size, data_length);
        }

        for (int i = 0; i < max_stream_idx; i++) {
            if (state.vertex_streams[i].data)
                // on the PS Vita, shader stores are used most of the time to write to a vertex buffer
                context.state.buffer_trapping.access_buffer(state.vertex_streams[i].data.address(), static_cast<uint32_t>(state.vertex_streams[i].size), mem, context.state.has_shader_store);
        }
    }

    if (max_stream_idx == 0)
        return;

    for (int i = 0; i < max_stream_idx; i++) {
        if (state.vertex_streams[i].data) {
            if (context.state.features.enable_memory_mapping) {
                auto [buffer, offset] = context.state.get_matching_mapping(state.vertex_streams[i].data.cast<void>());

                context.vertex_stream_offsets[i] = offset;
                context.vertex_stream_buffers[i] = buffer;
            } else {
                const uint8_t *stream = state.vertex_streams[i].data.get(mem);
                uint32_t stream_size = state.vertex_streams[i].size;
#ifdef __APPLE__
                // Vulkan allows any stride, but Metal only allows multiples of 4.
                const bool restride = vertex_program.streams[i].stride % 4 != 0;
                if (restride) {
                    restride_stream(stream, stream_size, vertex_program.streams[i].stride);
                }
#endif
                context.vertex_stream_ring_buffer.allocate(context.prerender_cmd, stream_size, stream);
                context.vertex_stream_offsets[i] = context.vertex_stream_ring_buffer.data_offset;

#ifdef __APPLE__
                if (restride) {
                    delete[] stream;
                }
#endif
            }

            state.vertex_streams[i].data = nullptr;
            state.vertex_streams[i].size = 0;
        }
    }

    context.render_cmd.bindVertexBuffers(0, max_stream_idx, context.vertex_stream_buffers, context.vertex_stream_offsets);
}

// Recognize this game's GPU-skinning vertex job and, if it matches, perform 4-bone linear-blend
// skinning on the CPU, writing the skinned position+normal into the output buffer in guest memory.
// Returns true if it handled the draw (the caller must then skip the GPU draw).
//
// Shape (decoded from shader 5120b121 / GXP reflection, verified against live captures):
//   - vertex program has the BUFFER_STORE flag (writes to memory via USSE STR)
//   - stream 0 (stride 24): position float3 @0, normal float3 @12
//   - stream 1 (stride 32): bone weights float4 @0, bone indices uint4 @16
//   - uniform buffer index 0 (host block 1): bone matrices, 48 bytes each (3x4 row-major affine)
//   - uniform buffer index 1 (host block 2): output buffer (position+normal, stride 24)
static bool try_cpu_vertex_job_skin(VKContext &context, SceGxmIndexFormat format, void *indices_ptr, size_t count, MemState &mem) {
    const SceGxmVertexProgram &vprog = *context.record.vertex_program.get(mem);
    const SceGxmProgram *vgxp = vprog.program.get(mem);
    if (!vgxp || !(vgxp->program_flags & SCE_GXM_PROGRAM_FLAG_BUFFER_STORE))
        return false;

    // Match the exact attribute/stream signature so we never mis-handle some other store shader.
    if (vprog.streams.size() != 2 || vprog.attributes.size() != 4)
        return false;
    if (vprog.streams[0].stride != 24 || vprog.streams[1].stride != 32)
        return false;

    const uint8_t *s0 = context.record.vertex_streams[0].data.get(mem);
    const uint8_t *s1 = context.record.vertex_streams[1].data.get(mem);
    const float *mats = Ptr<const float>(g_vertex_uniform_block_addr[1]).get(mem);   // bone matrices (block 1)
    uint8_t *outbuf = Ptr<uint8_t>(g_vertex_uniform_block_addr[2]).get(mem);          // output buffer (block 2)
    if (!s0 || !s1 || !mats || !outbuf)
        return false;

    const uint32_t s0_stride = vprog.streams[0].stride;
    const uint32_t s1_stride = vprog.streams[1].stride;
    constexpr uint32_t out_stride = 24; // position float3 + normal float3
    constexpr uint32_t kMaxBones = 4096; // sanity cap so a mis-detected draw can't OOB-read the matrix block

    const auto skin_vertex = [&](uint32_t v) {
        const float *in = reinterpret_cast<const float *>(s0 + static_cast<size_t>(v) * s0_stride);
        const float *wgt = reinterpret_cast<const float *>(s1 + static_cast<size_t>(v) * s1_stride);
        const uint32_t *idx = reinterpret_cast<const uint32_t *>(s1 + static_cast<size_t>(v) * s1_stride + 16);
        const float px = in[0], py = in[1], pz = in[2];
        const float nx = in[3], ny = in[4], nz = in[5];
        float op[3] = { 0.f, 0.f, 0.f };
        float on[3] = { 0.f, 0.f, 0.f };
        for (int i = 0; i < 4; i++) {
            const float w = wgt[i];
            if (w == 0.0f || idx[i] >= kMaxBones)
                continue;
            const float *m = mats + static_cast<size_t>(idx[i]) * 12; // 12 floats = 3x4 affine
            for (int r = 0; r < 3; r++) {
                const float r0 = m[r * 4 + 0], r1 = m[r * 4 + 1], r2 = m[r * 4 + 2], rt = m[r * 4 + 3];
                op[r] += w * (r0 * px + r1 * py + r2 * pz + rt);
                on[r] += w * (r0 * nx + r1 * ny + r2 * nz);
            }
        }
        float *out = reinterpret_cast<float *>(outbuf + static_cast<size_t>(v) * out_stride);
        out[0] = op[0]; out[1] = op[1]; out[2] = op[2];
        out[3] = on[0]; out[4] = on[1]; out[5] = on[2];
    };

    const auto index_at = [&](size_t i) -> uint32_t {
        if (!indices_ptr)
            return static_cast<uint32_t>(i);
        return (format == SCE_GXM_INDEX_FORMAT_U16)
            ? reinterpret_cast<const uint16_t *>(indices_ptr)[i]
            : reinterpret_cast<const uint32_t *>(indices_ptr)[i];
    };

    // Confirm stream 1 really carries normalized bone weights + sane indices for THIS draw, not just
    // a shader whose attribute shape happens to match. Genuine skinning weights sum to ~1. This is the
    // key gate that fixes the c1c65cf9 regression: other BUFFER_STORE shaders (and any draw bound while
    // the global block addresses are stale) fail this check, so we neither skip nor corrupt their draws.
    {
        const size_t sample_n = std::min<size_t>(count, 8);
        int valid = 0;
        for (size_t i = 0; i < sample_n; i++) {
            const uint32_t v = index_at(i);
            const float *wgt = reinterpret_cast<const float *>(s1 + static_cast<size_t>(v) * s1_stride);
            const uint32_t *idx = reinterpret_cast<const uint32_t *>(s1 + static_cast<size_t>(v) * s1_stride + 16);
            float sum = 0.f;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
                const float w = wgt[k];
                if (!(w >= -0.02f && w <= 1.02f))
                    ok = false;
                if (idx[k] >= kMaxBones)
                    ok = false;
                sum += w;
            }
            if (ok && sum > 0.85f && sum < 1.15f)
                valid++;
        }
        if (sample_n == 0 || valid * 2 < static_cast<int>(sample_n)) {
            // Shape matched but stream 1 isn't normalized bone weights — diagnostic to tell apart
            // "validation too strict (character would vanish)" from "correctly skipped a non-skinning draw".
            static std::once_flag rej_once;
            std::call_once(rej_once, [&] { LOG_INFO("[SKIN] store-shader matched shape but stream1 != bone weights (sampled {}, valid {}) — using normal draw", (int)sample_n, valid); });
            return false; // not a real skinning draw — let the normal GPU path handle it
        }
    }

    if (indices_ptr) {
        if (format == SCE_GXM_INDEX_FORMAT_U16) {
            const uint16_t *ip = reinterpret_cast<const uint16_t *>(indices_ptr);
            for (size_t i = 0; i < count; i++)
                skin_vertex(ip[i]);
        } else {
            const uint32_t *ip = reinterpret_cast<const uint32_t *>(indices_ptr);
            for (size_t i = 0; i < count; i++)
                skin_vertex(ip[i]);
        }
    } else {
        for (size_t v = 0; v < count; v++)
            skin_vertex(static_cast<uint32_t>(v));
    }

    static std::once_flag once;
    std::call_once(once, [&]() { LOG_INFO("[SKIN] CPU vertex-job skinning active (e.g. {} verts)", (int)count); });
    return true;
}

void draw(VKContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    Ptr<void> indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config) {
    void *indices_ptr = indices.get(mem);

    context.check_for_macroblock_change(true);

    // CPU emulation of GPU-skinning "vertex jobs". This game (and Unity titles in general) skin
    // characters with a vertex program that loads bone matrices and WRITES the transformed
    // position/normal back to a buffer via USSE STR. That requires GPU memory mapping, which on
    // macOS/MoltenVK ends up crashing Apple's Metal shader compiler. Instead we recognize the
    // shape of this shader and run linear-blend skinning on the CPU, writing the output buffer in
    // guest memory; the subsequent normal MVP draw then renders the skinned mesh. This keeps
    // memory mapping disabled and avoids the Metal compiler entirely.
    //
    // Default ON. An earlier version (c1c65cf9) regressed other rendering because the match was too
    // loose: non-skinning BUFFER_STORE draws were intercepted and skipped (objects vanished) and
    // skinned output was written through stale GLOBAL block addresses (2D NPC body parts vanished,
    // 3D floor went black). try_cpu_vertex_job_skin now validates that stream 1 actually holds
    // normalized bone weights + sane indices before acting, so only genuine skinning draws are
    // handled and everything else falls through to the normal GPU path. Set V3K_CPU_SKIN=0 to disable.
    static const bool cpu_skin_enabled = [] {
        const char *e = std::getenv("V3K_CPU_SKIN");
        return !e || e[0] != '0';
    }();
    if (cpu_skin_enabled && try_cpu_vertex_job_skin(context, format, indices_ptr, count, mem))
        return;

    if (!context.in_renderpass)
        context.start_render_pass();

    // when we do multiple render pass for one scene (shader interlock or slow macroblock),
    // we need to always load the depth-stencil after the first draw
    if (context.is_first_scene_draw && (context.state.features.support_shader_interlock || context.ignore_macroblock)) {
        // update the render pass to load and store the depth and stencil
        context.current_render_pass = context.state.pipeline_cache.retrieve_render_pass(context.current_color_format, true, true, !context.record.color_surface.data);
        context.is_first_scene_draw = false;
    }

    const SceGxmFragmentProgram &gxm_fragment_program = *context.record.fragment_program.get(mem);
    const SceGxmProgram &fragment_program_gxp = *gxm_fragment_program.program.get(mem);
    if (context.state.features.direct_fragcolor && fragment_program_gxp.is_frag_color_used()) {
        // the fragment shader is using programmable blending with a subpass input
        vk::ImageMemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = context.current_color_base_image->image,
            .subresourceRange = vkutil::color_subresource_range
        };
        context.render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlagBits::eByRegion, {}, {}, barrier);
    } else if (context.state.features.support_shader_interlock
        && fragment_program_gxp.is_frag_color_used() != context.last_draw_was_framebuffer_fetch) {
        // restart the render pass to act as a barrier
        context.render_cmd.endRenderPass();

        if (fragment_program_gxp.is_frag_color_used()) {
            context.curr_renderpass_info.framebuffer = context.current_shader_interlock_framebuffer;
            context.curr_renderpass_info.renderPass = context.current_shader_interlock_pass;
        } else {
            context.curr_renderpass_info.framebuffer = context.current_framebuffer;
            context.curr_renderpass_info.renderPass = context.current_render_pass;
        }

        context.render_cmd.beginRenderPass(context.curr_renderpass_info, vk::SubpassContents::eInline);
        context.last_draw_was_framebuffer_fetch = fragment_program_gxp.is_frag_color_used();
    }

    if (context.current_visibility_buffer != nullptr && context.current_query_idx != -1 && !context.is_in_query) {
        if (context.current_visibility_buffer->queries_used[context.current_query_idx]) {
            LOG_WARN_ONCE("Visibility buffer entry is used more than once in a scene");
            // still let this happen, this is a validation error but I think most GPUs should be fine with it
        }
        context.current_visibility_buffer->queries_used[context.current_query_idx] = true;

        context.visibility_max_used_idx = std::max(context.visibility_max_used_idx, context.current_query_idx);

        const vk::QueryControlFlags control_flags = (context.is_query_op_increment && context.state.physical_device_features.occlusionQueryPrecise) ? vk::QueryControlFlagBits::ePrecise : vk::QueryControlFlags();
        context.render_cmd.beginQuery(context.current_visibility_buffer->query_pool, context.current_query_idx, control_flags);
        context.is_in_query = true;
    }

    // do we need to check for a pipeline change?
    if (context.refresh_pipeline || type != context.last_primitive) {
        context.refresh_pipeline = false;
        context.last_primitive = type;

        // We don't want to defer cases where we draw a whole quad over the screen as these draws could be necessary
        // to be able to see anything
        bool can_be_whole_quad = instance_count == 1 && count <= 6;
        vk::Pipeline new_pipeline = context.state.pipeline_cache.retrieve_pipeline(context, type, !can_be_whole_quad, mem);

        if (new_pipeline != context.current_pipeline) {
            context.current_pipeline = new_pipeline;

            if (new_pipeline != nullptr)
                context.render_cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, context.current_pipeline);
        }
    }

    // can happen with asynchronous pipeline compilation
    if (context.current_pipeline == nullptr)
        return;

    if (config.log_active_shaders) {
        const std::string hash_text_f = hex_string(context.record.fragment_program.get(mem)->renderer_data->hash);
        const std::string hash_text_v = hex_string(context.record.vertex_program.get(mem)->renderer_data->hash);

        LOG_DEBUG("\nVertex  : {}\nFragment: {}", hash_text_v, hash_text_f);
        LOG_DEBUG("Vertex default uniform buffer: {}\n", spdlog::to_hex(context.ubo_data[0], 16));
        LOG_DEBUG("Fragment default uniform buffer: {}\n", spdlog::to_hex(context.ubo_data[SCE_GXM_REAL_MAX_UNIFORM_BUFFER], 16));
    }

    const bool use_memory_mapping = context.state.features.enable_memory_mapping;

    // update uniforms if needed
    // first update the buffer and texture count
    auto &vert_render_data = context.record.vertex_program.get(mem)->renderer_data;
    auto &frag_render_data = context.record.fragment_program.get(mem)->renderer_data;

    if (use_memory_mapping) {
        context.curr_vert_ublock.set_buffer_count(vert_render_data->buffer_count);
        context.curr_frag_ublock.set_buffer_count(frag_render_data->buffer_count);
    }

    if (context.state.features.use_texture_viewport) {
        context.curr_vert_ublock.set_texture_count(vert_render_data->texture_count);
        context.curr_frag_ublock.set_texture_count(frag_render_data->texture_count);
    }

    auto &vert_ublock = context.curr_vert_ublock.base_block;
    vert_ublock.viewport_flip = context.record.viewport_flip;
    vert_ublock.viewport_flag = (context.record.viewport_flat) ? 0.0f : 1.0f;
    vert_ublock.z_offset = context.record.z_offset;
    vert_ublock.z_scale = context.record.z_scale;
    vert_ublock.screen_width = context.render_target->width / context.state.res_multiplier;
    vert_ublock.screen_height = context.render_target->height / context.state.res_multiplier;

    if (context.curr_vert_ublock.changed || memcmp(&context.prev_vert_ublock, &vert_ublock, sizeof(vert_ublock)) != 0) {
        // TODO: this intermediate step can be avoided
        context.curr_vert_ublock.copy_to(context.shader_info_temp);
        context.vertex_info_uniform_buffer.allocate(context.prerender_cmd, context.curr_vert_ublock.get_size(), context.shader_info_temp);
        memcpy(&context.prev_vert_ublock, &vert_ublock, sizeof(vert_ublock));
    }

    auto &frag_ublock = context.curr_frag_ublock.base_block;
    frag_ublock.writing_mask = context.record.writing_mask;
    frag_ublock.res_multiplier = context.state.res_multiplier;
    const bool has_msaa = context.render_target->multisample_mode;
    const bool has_downscale = context.record.color_surface.downscale;
    if (has_msaa && !has_downscale)
        frag_ublock.res_multiplier *= 2;
    else if (!has_msaa && has_downscale)
        frag_ublock.res_multiplier /= 2;

    if (context.curr_frag_ublock.changed || memcmp(&context.prev_frag_ublock, &frag_ublock, sizeof(frag_ublock)) != 0) {
        // TODO: this intermediate step can be avoided
        context.curr_frag_ublock.copy_to(context.shader_info_temp);
        context.fragment_info_uniform_buffer.allocate(context.prerender_cmd, context.curr_frag_ublock.get_size(), context.shader_info_temp);
        memcpy(&context.prev_frag_ublock, &frag_ublock, sizeof(frag_ublock));
    }

    // create, update and bind descriptors (uniforms and textures)
    draw_bind_descriptors(context, mem);

    // Upload index data.
    vk::IndexType index_type = (format == SCE_GXM_INDEX_FORMAT_U16) ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const size_t index_size = (format == SCE_GXM_INDEX_FORMAT_U16) ? 2 : 4;

    uint32_t max_index = 0;
    if (use_memory_mapping) {
        auto [buffer, offset] = context.state.get_matching_mapping(indices);
        if (context.state.mapping_method == MappingMethod::DoubleBuffer) {
            TrappedBuffer *trapped_buffer = context.state.buffer_trapping.access_buffer(indices.address(), count * index_size, mem);
            if (trapped_buffer->extra == ~0) {
                // store the max element in extra
                if (format == SCE_GXM_INDEX_FORMAT_U16) {
                    uint16_t *data = indices.cast<uint16_t>().get(mem);
                    trapped_buffer->extra = *std::max_element(&data[0], &data[count]);
                } else {
                    uint32_t *data = indices.cast<uint32_t>().get(mem);
                    trapped_buffer->extra = *std::max_element(&data[0], &data[count]);
                }
            }
            max_index = trapped_buffer->extra;
        }
        context.render_cmd.bindIndexBuffer(buffer, offset, index_type);
    } else {
        const size_t index_buffer_size = index_size * count;
        context.index_stream_ring_buffer.allocate(context.prerender_cmd, index_buffer_size, indices_ptr);
        context.render_cmd.bindIndexBuffer(context.index_stream_ring_buffer.handle(), context.index_stream_ring_buffer.data_offset, index_type);
    }

    // bind the vertex streams
    bind_vertex_streams(context, mem, instance_count, max_index);

    context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);

    context.vertex_uniform_storage_allocated = false;
    context.fragment_uniform_storage_allocated = false;
}

} // namespace renderer::vulkan
