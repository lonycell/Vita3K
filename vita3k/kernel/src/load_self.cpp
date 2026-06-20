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

#include <cpu/functions.h>
#include <kernel/load_self.h>
#include <kernel/relocation.h>
#include <kernel/state.h>
#include <kernel/types.h>

#include <nids/functions.h>
#include <util/arm.h>
#include <util/fs.h>
#include <util/log.h>

#include <util/elf.h>
// clang-format off
#define SCE_ELF_DEFS_TARGET
#include <sce-elf-defs.h>
#undef SCE_ELF_DEFS_TARGET
// clang-format on
#include <miniz.h>
#include <self.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>
#include <fstream>
#include <iomanip>
#include <iostream>

static constexpr uint32_t NID_MODULE_STOP = 0x79F8E492;
static constexpr uint32_t NID_MODULE_EXIT = 0x913482A9;
static constexpr uint32_t NID_MODULE_START = 0x935CD196;
static constexpr uint32_t NID_MODULE_INFO = 0x6C2224BA;
static constexpr uint32_t NID_SYSLYB = 0x936c8a78;
static constexpr uint32_t NID_PROCESS_PARAM = 0x70FBA1E7;

static constexpr bool LOG_MODULE_LOADING = false;

struct VarImportsHeader {
    uint32_t unk : 4; // Must be zero
    uint32_t reloc_data_size : 24; // Size of Relocation data in bytes, includes this header.
    uint32_t unk2 : 4; // Must be zero
};
static_assert(sizeof(VarImportsHeader) == sizeof(uint32_t));

static bool load_var_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, const SegmentInfosForReloc &segments, KernelState &kernel, MemState &mem, uint32_t module_id) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (kernel.debugger.log_imports) {
            const char *const name = import_name(nid);
            LOG_DEBUG("\tNID {} ({}). entry: {}, *entry: {}", log_hex(nid), name, entry, log_hex(*entry.get(mem)));
        }

        VarImportsHeader *const var_reloc_header = entry.cast<VarImportsHeader>().get(mem);
        const auto var_reloc_entries = static_cast<void *>(var_reloc_header + 1);
        const uint32_t reloc_size = (var_reloc_header->reloc_data_size > sizeof(VarImportsHeader)) ? (var_reloc_header->reloc_data_size - sizeof(VarImportsHeader)) : 0;

        const char *const name = import_name(nid);
        Address export_address;
        kernel.var_binding_infos.emplace(nid, VarBindingInfo{ var_reloc_entries, reloc_size, module_id });
        const ExportNids::iterator export_address_it = kernel.export_nids.find(nid);
        if (export_address_it != kernel.export_nids.end()) {
            export_address = export_address_it->second;
        } else {
            constexpr auto STUB_SYMVAL = 0xDEADBEEF;
            LOG_DEBUG("\tNID NOT FOUND {} ({}) at {}, setting to stub value {}", log_hex(nid), name, log_hex(entry.address()), log_hex(STUB_SYMVAL));

            auto alloc_name = fmt::format("Stub var import reloc symval, NID {} ({})", log_hex(nid), name);
            auto stub_symval_ptr = Ptr<uint32_t>(alloc(mem, 4, alloc_name.c_str()));
            *stub_symval_ptr.get(mem) = STUB_SYMVAL;

            export_address = stub_symval_ptr.address();

            // Use same stub for other var imports
            kernel.export_nids.emplace(nid, export_address);
        }

        if (reloc_size)
            if (!relocate(var_reloc_entries, reloc_size, segments, mem, true, export_address))
                return false;
    }

    return true;
}

static bool unload_var_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel, MemState &mem, uint32_t module_id) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        VarImportsHeader *const var_reloc_header = reinterpret_cast<VarImportsHeader *>(entry.get(mem));
        const auto var_reloc_entries = static_cast<void *>(var_reloc_header + 1);
        const uint32_t reloc_size = (var_reloc_header->reloc_data_size > sizeof(VarImportsHeader)) ? (var_reloc_header->reloc_data_size - sizeof(VarImportsHeader)) : 0;

        // remove the binding info from the map
        VarBindingInfo binding_info{ var_reloc_entries, reloc_size, module_id };
        auto range = kernel.var_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            if (memcmp(&it->second, &binding_info, sizeof(VarBindingInfo)) == 0) {
                kernel.var_binding_infos.erase(it);
                break;
            }
        }
    }

    return true;
}

static bool load_func_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, const SegmentInfosForReloc &segments, KernelState &kernel, const MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (kernel.debugger.log_imports || std::getenv("V3K_LOG_IMPORTS")) {
            const char *const name = import_name(nid);
            LOG_INFO("\tNID {} ({}) at {}", log_hex(nid), name, log_hex(entry.address()));
        }

        const ExportNids::iterator export_address = kernel.export_nids.find(nid);
        uint32_t *const stub = entry.get(mem);

        kernel.func_binding_infos.emplace(nid, entry.address());
        if (export_address == kernel.export_nids.end()) {
            stub[0] = 0xef000000; // svc #0 - Call our interrupt hook.
            stub[1] = 0xe1a0f00e; // mov pc, lr - Return to the caller.
            stub[2] = nid; // Our interrupt hook will read this.
        } else {
            Address func_address = export_address->second;
            stub[0] = encode_arm_inst(INSTRUCTION_MOVW, (uint16_t)func_address, 12);
            stub[1] = encode_arm_inst(INSTRUCTION_MOVT, (uint16_t)(func_address >> 16), 12);
            stub[2] = encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
        }
        if (stub[3]) { // if function's associated reftable exists
            VarImportsHeader *const var_reloc_header = Ptr<VarImportsHeader>(stub[3]).get(mem);
            const auto var_reloc_entries = static_cast<void *>(var_reloc_header + 1);
            const uint32_t reloc_size = (var_reloc_header->reloc_data_size > sizeof(VarImportsHeader)) ? (var_reloc_header->reloc_data_size - sizeof(VarImportsHeader)) : 0;
            if (reloc_size) {
                if (!relocate(var_reloc_entries, reloc_size, segments, mem, true, entry.address())) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool unload_func_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        // remove the stub from the table
        auto range = kernel.func_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == entry.address()) {
                kernel.func_binding_infos.erase(it);
                break;
            }
        }
    }
    return true;
}

static bool load_imports(const sce_module_info_raw &module, Ptr<const void> segment_address, const SegmentInfosForReloc &segments, KernelState &kernel, MemState &mem, bool is_unload = false) {
    const uint8_t *const base = segment_address.cast<const uint8_t>().get(mem);
    const sce_module_imports_raw *const imports_begin = reinterpret_cast<const sce_module_imports_raw *>(base + module.import_top);
    const sce_module_imports_raw *const imports_end = reinterpret_cast<const sce_module_imports_raw *>(base + module.import_end);

    for (const sce_module_imports_raw *imports = imports_begin; imports < imports_end; imports = reinterpret_cast<const sce_module_imports_raw *>(reinterpret_cast<const uint8_t *>(imports) + imports->size)) {
        assert(imports->num_syms_tls_vars == 0);

        Address library_name{};
        Address func_nid_table{};
        Address func_entry_table{};
        Address var_nid_table{};
        Address var_entry_table{};

        if (imports->size == 0x24) {
            auto short_imports = reinterpret_cast<const sce_module_imports_short_raw *>(imports);
            library_name = short_imports->library_name;
            func_nid_table = short_imports->func_nid_table;
            func_entry_table = short_imports->func_entry_table;
            var_nid_table = short_imports->var_nid_table;
            var_entry_table = short_imports->var_entry_table;
        } else if (imports->size == 0x34) {
            auto long_imports = imports;
            library_name = long_imports->library_name;
            func_nid_table = long_imports->func_nid_table;
            func_entry_table = long_imports->func_entry_table;
            var_nid_table = long_imports->var_nid_table;
            var_entry_table = long_imports->var_entry_table;
        }

        std::string lib_name;
        if (kernel.debugger.log_imports || std::getenv("V3K_LOG_IMPORTS")) {
            lib_name = Ptr<const char>(library_name).get(mem);
            LOG_INFO("Loading func imports from {}", lib_name);
        }

        const uint32_t *const nids = Ptr<const uint32_t>(func_nid_table).get(mem);
        const Ptr<uint32_t> *const entries = Ptr<Ptr<uint32_t>>(func_entry_table).get(mem);

        const size_t num_syms_funcs = imports->num_syms_funcs;
        if (!is_unload && !load_func_imports(nids, entries, num_syms_funcs, segments, kernel, mem))
            return false;
        if (is_unload && !unload_func_imports(nids, entries, num_syms_funcs, kernel))
            return false;

        const uint32_t *const var_nids = Ptr<const uint32_t>(var_nid_table).get(mem);
        const Ptr<uint32_t> *const var_entries = Ptr<Ptr<uint32_t>>(var_entry_table).get(mem);

        const auto var_count = imports->num_syms_vars;

        if (kernel.debugger.log_imports && var_count > 0)
            LOG_INFO("Loading var imports from {}", lib_name);

        if (!is_unload && !load_var_imports(var_nids, var_entries, var_count, segments, kernel, mem, module.module_nid))
            return false;
        if (is_unload && !unload_var_imports(var_nids, var_entries, var_count, kernel, mem, module.module_nid))
            return false;
    }

    return true;
}

static bool load_func_exports(SceKernelModuleInfo *kernel_module_info, const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (nid == NID_MODULE_START) {
            kernel_module_info->start_entry = entry;
            continue;
        }

        if (nid == NID_MODULE_STOP) {
            kernel_module_info->stop_entry = entry;
            continue;
        }
        if (nid == NID_MODULE_EXIT) {
            kernel_module_info->exit_entry = entry;
            continue;
        }

        kernel.export_nids.emplace(nid, entry.address());
        if (std::getenv("V3K_LOG_EXPORTS"))
            LOG_INFO("[EXPORT] {} NID 0x{:08X} -> 0x{:08X}", kernel_module_info->module_name, nid, entry.address());
        // substitute supervisor calls to direct function calls in loaded modules
        auto range = kernel.func_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            auto address = it->second;
            uint32_t *const stub = Ptr<uint32_t>(address).get(mem);
            stub[0] = encode_arm_inst(INSTRUCTION_MOVW, (uint16_t)entry.address(), 12);
            stub[1] = encode_arm_inst(INSTRUCTION_MOVT, (uint16_t)(entry.address() >> 16), 12);
            stub[2] = encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
            kernel.invalidate_jit_cache(address, 3 * sizeof(uint32_t));
        }

        if (kernel.debugger.log_exports) {
            const char *const name = import_name(nid);

            LOG_DEBUG("\tNID {} ({}) at {}", log_hex(nid), name, log_hex(entry.address()));
        }
    }

    return true;
}

static bool unload_func_exports(const uint32_t *nids, size_t count, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];

        if (nid == NID_MODULE_START || nid == NID_MODULE_STOP || nid == NID_MODULE_EXIT)
            continue;

        kernel.export_nids.erase(nid);
        // invalidate all lle nid calls
        auto range = kernel.func_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            Address entry = it->second;
            uint32_t *stub = Ptr<uint32_t>(entry).get(mem);

            stub[0] = 0xef000000; // svc #0 - Call our interrupt hook.
            stub[1] = 0xe1a0f00e; // mov pc, lr - Return to the caller.
            stub[2] = nid; // Our interrupt hook will read this.
            kernel.invalidate_jit_cache(entry, 3 * sizeof(uint32_t));
        }
    }

    return true;
}

static bool load_var_exports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (nid == NID_PROCESS_PARAM) {
            if (!kernel.process_param)
                kernel.load_process_param(mem, entry);
            LOG_DEBUG("\tNID {} (SCE_PROC_PARAMS) at {}", log_hex(nid), log_hex(entry.address()));
            continue;
        }

        if (nid == NID_MODULE_INFO) {
            LOG_DEBUG("\tNID {} (NID_MODULE_INFO) at {}", log_hex(nid), log_hex(entry.address()));
            continue;
        }

        if (nid == NID_SYSLYB) {
            LOG_DEBUG("\tNID {} (SYSLYB) at {}", log_hex(nid), log_hex(entry.address()));
            continue;
        }

        if (kernel.debugger.log_exports) {
            const char *const name = import_name(nid);

            LOG_DEBUG("\tNID {} ({}) at {}", log_hex(nid), name, log_hex(entry.address()));
        }

        Address old_entry_address = 0;
        auto nid_it = kernel.export_nids.find(nid);
        if (nid_it != kernel.export_nids.end()) {
            LOG_DEBUG("Found previously not found variable. nid:{}, new_entry_point:{}", log_hex(nid), log_hex(entry.address()));
            old_entry_address = kernel.export_nids[nid];
        }
        kernel.export_nids[nid] = entry.address();

        auto range = kernel.var_binding_infos.equal_range(nid);
        for (auto j = range.first; j != range.second; ++j) {
            auto &var_binding_info = j->second;
            if (var_binding_info.size == 0)
                continue;

            SegmentInfosForReloc seg;
            const auto &module_info = kernel.loaded_modules[kernel.module_uid_by_nid[var_binding_info.module_nid]];
            if (!module_info) {
                LOG_ERROR("Module not found by nid: {} uid: {}", log_hex(var_binding_info.module_nid), kernel.module_uid_by_nid[var_binding_info.module_nid]);
            } else {
                for (int k = 0; k < MODULE_INFO_NUM_SEGMENTS; k++) {
                    const auto &segment = module_info->info.segments[k];
                    if (segment.size > 0) {
                        seg[k] = { segment.vaddr.address(), 0, segment.memsz }; // p_vaddr is not used in variable relocations
                    }
                }
            }

            // Note: We make the assumption that variables are not imported into executable code (wouldn't make a lot of sense)
            // If this is not the case, uncomment the following
            /* if (!seg.empty()) {
                for (const auto &[key, value] : seg) {
                    kernel.invalidate_jit_cache(value.addr, value.size);
                }
            }*/

            if (!seg.empty()) {
                if (!relocate(var_binding_info.entries, var_binding_info.size, seg, mem, true, entry.address())) {
                    LOG_ERROR("Failed to relocate late binding info");
                }
            }
        }
        if (old_entry_address)
            free(mem, old_entry_address);
    }
    return true;
}

static bool unload_var_exports(const uint32_t *nids, size_t count, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];

        if (nid == NID_PROCESS_PARAM || nid == NID_MODULE_INFO || nid == NID_SYSLYB)
            continue;

        // replace again the nid by a stub
        constexpr auto STUB_SYMVAL = 0xDEADBEEF;

        auto alloc_name = fmt::format("Stub var import reloc symval, NID {} ({})", log_hex(nid), import_name(nid));
        auto stub_symval_ptr = Ptr<uint32_t>(alloc(mem, 4, alloc_name.c_str()));
        *stub_symval_ptr.get(mem) = STUB_SYMVAL;

        const Ptr<uint32_t> entry = stub_symval_ptr;

        // Use same stub for other var imports
        kernel.export_nids[nid] = entry.address();

        auto range = kernel.var_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            auto &var_binding_info = it->second;
            if (var_binding_info.size == 0)
                continue;

            SegmentInfosForReloc seg;
            const auto &module_info = kernel.loaded_modules[kernel.module_uid_by_nid[var_binding_info.module_nid]];
            if (!module_info) {
                LOG_ERROR("Module not found by nid: {} uid: {}", log_hex(var_binding_info.module_nid), kernel.module_uid_by_nid[var_binding_info.module_nid]);
            } else {
                for (int k = 0; k < MODULE_INFO_NUM_SEGMENTS; k++) {
                    const auto &segment = module_info->info.segments[k];
                    if (segment.size > 0) {
                        seg[k] = { segment.vaddr.address(), 0, segment.memsz }; // p_vaddr is not used in variable relocations
                    }
                }
            }

            if (!seg.empty()) {
                if (!relocate(var_binding_info.entries, var_binding_info.size, seg, mem, true, entry.address())) {
                    LOG_ERROR("Failed to relocate late binding info");
                }
            }
        }
    }
    return true;
}

static bool load_exports(SceKernelModuleInfo *kernel_module_info, const sce_module_info_raw &module, Ptr<const void> segment_address, KernelState &kernel, MemState &mem, bool is_unload = false) {
    const uint8_t *const base = segment_address.cast<const uint8_t>().get(mem);
    const sce_module_exports_raw *const exports_begin = reinterpret_cast<const sce_module_exports_raw *>(base + module.export_top);
    const sce_module_exports_raw *const exports_end = reinterpret_cast<const sce_module_exports_raw *>(base + module.export_end);

    for (const sce_module_exports_raw *exports = exports_begin; exports < exports_end; exports = reinterpret_cast<const sce_module_exports_raw *>(reinterpret_cast<const uint8_t *>(exports) + exports->size)) {
        const char *const lib_name = Ptr<const char>(exports->library_name).get(mem);

        if (kernel.debugger.log_exports)
            LOG_INFO("Loading func exports from {}", lib_name ? lib_name : "unknown");

        const uint32_t *const nids = Ptr<const uint32_t>(exports->nid_table).get(mem);
        const Ptr<uint32_t> *const entries = Ptr<Ptr<uint32_t>>(exports->entry_table).get(mem);
        if (!is_unload && !load_func_exports(kernel_module_info, nids, entries, exports->num_syms_funcs, kernel, mem))
            return false;
        if (is_unload && !unload_func_exports(nids, exports->num_syms_funcs, kernel, mem))
            return false;

        const auto var_count = exports->num_syms_vars;

        if (kernel.debugger.log_exports && var_count > 0) {
            LOG_INFO("Loading var exports from {}", lib_name ? lib_name : "unknown");
        }

        if (!is_unload && !load_var_exports(&nids[exports->num_syms_funcs], &entries[exports->num_syms_funcs], var_count, kernel, mem))
            return false;
        if (is_unload && !unload_var_exports(&nids[exports->num_syms_funcs], var_count, kernel, mem))
            return false;
    }

    return true;
}

/**
 * \return Negative on failure
 */
SceUID load_self(KernelState &kernel, MemState &mem, const void *self, const std::string &self_path, const fs::path &dump_path) {
    const uint8_t *const image_bytes = static_cast<const uint8_t *>(self);
    const SCE_header &self_header = *static_cast<const SCE_header *>(self);

    constexpr uint32_t SCE_MAGIC = 0x00454353; // "SCE\0"
    const bool is_self = (self_header.magic == SCE_MAGIC);

    if (is_self) {
        // assumes little endian host
        if (self_header.version != 3) {
            LOG_CRITICAL("SELF {} version {} is not supported.", self_path, self_header.version);
            return -1;
        }

        if (self_header.header_type != 1) {
            LOG_CRITICAL("SELF {} header type {} is not supported.", self_path, self_header.header_type);
            return -1;
        }

        if (self_path == "app0:sce_module/steroid.suprx") {
            LOG_CRITICAL("You're trying to load a vitamin dump. It is not supported.");
            return -1;
        }
    }

    const uint8_t *const elf_bytes = is_self ? (image_bytes + self_header.elf_offset) : image_bytes;
    const Elf32_Ehdr &elf = *reinterpret_cast<const Elf32_Ehdr *>(elf_bytes);
    const uint32_t module_info_offset = elf.e_entry & 0x3fffffff;
    const Elf32_Phdr *const segments = is_self
        ? reinterpret_cast<const Elf32_Phdr *>(image_bytes + self_header.phdr_offset)
        : reinterpret_cast<const Elf32_Phdr *>(elf_bytes + elf.e_phoff);
    const segment_info *const seg_infos = is_self
        ? reinterpret_cast<const segment_info *>(image_bytes + self_header.section_info_offset)
        : nullptr;

    // Verify ELF header is correct
    if (!EHDR_HAS_VALID_MAGIC(elf)) {
        LOG_CRITICAL("Cannot load file {}: invalid ELF magic.", self_path);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_ident[EI_CLASS] != ELFCLASS32) {
        LOG_CRITICAL("Cannot load ELF {}: unexpected EI_CLASS {}.", self_path, elf.e_ident[EI_CLASS]);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_ident[EI_DATA] != ELFDATA2LSB) {
        LOG_CRITICAL("Cannot load ELF {}: unexpected EI_DATA {}.", self_path, elf.e_ident[EI_DATA]);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_ident[EI_VERSION] != EV_CURRENT) {
        LOG_CRITICAL("Cannot load ELF {}: invalid EI_VERSION {}.", self_path, elf.e_ident[EI_VERSION]);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_machine != EM_ARM) {
        LOG_CRITICAL("Cannot load ELF {}: unexpected e_machine {}.", self_path, elf.e_machine);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    // log elf header
    LOG_TRACE("ELF Header: e_type: {}, e_machine: {}, e_version: {}, e_entry: {}, e_phoff: {}, e_shoff: {}, e_flags: {}, e_ehsize: {}, e_phentsize: {}, e_phnum: {}, e_shentsize: {}, e_shnum: {}, e_shstrndx: {}",
        log_hex(elf.e_type), log_hex(elf.e_machine), log_hex(elf.e_version), log_hex(elf.e_entry), log_hex(elf.e_phoff), log_hex(elf.e_shoff), log_hex(elf.e_flags), log_hex(elf.e_ehsize), log_hex(elf.e_phentsize), log_hex(elf.e_phnum), log_hex(elf.e_shentsize), log_hex(elf.e_shnum), log_hex(elf.e_shstrndx));

    bool isRelocatable;
    if (elf.e_type == ET_SCE_EXEC) {
        isRelocatable = false;
    } else if (elf.e_type == ET_SCE_RELEXEC) {
        isRelocatable = true;
    } else if (elf.e_type == ET_SCE_PSP2RELEXEC) {
        LOG_CRITICAL("Cannot load ELF {}: ET_SCE_PSP2RELEXEC is not supported.", self_path);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    } else {
        LOG_CRITICAL("Cannot load ELF {}: unexpected e_type {}.", self_path, elf.e_type);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    // TODO: is OSABI always 0?
    // TODO: is ABI_VERSION always 0?

    if (is_self) {
        LOG_DEBUG_IF(LOG_MODULE_LOADING, "Loading SELF at {}... (ELF type: {}, self_filesize: {}, self_offset: {}, module_info_offset: {})", self_path, log_hex(elf.e_type), log_hex(self_header.self_filesize), log_hex(self_header.self_offset), log_hex(module_info_offset));
    } else {
        LOG_DEBUG_IF(LOG_MODULE_LOADING, "Loading ELF at {}... (ELF type: {}, module_info_offset: {})", self_path, log_hex(elf.e_type), log_hex(module_info_offset));
    }

    auto get_seg_header_string = [](uint32_t p_type) {
        if (p_type == PT_NULL) {
            return "NULL";
        } else if (p_type == PT_LOAD) {
            return "LOAD";
        } else if (p_type == PT_SCE_COMMENT) {
            return "SCE Comment";
        } else if (p_type == PT_SCE_VERSION) {
            return "SCE Version";
        } else if ((PT_LOOS <= p_type) && (p_type <= PT_HIOS)) {
            return "OS-specific";
        } else if ((PT_LOPROC <= p_type) && (p_type <= PT_HIPROC)) {
            return "Processor-specific";
        } else {
            return "Unknown";
        }
    };

    SegmentInfosForReloc segment_reloc_info;

    auto free_all_segments = [](MemState &mem, SegmentInfosForReloc &segs_info) {
        for (auto &[_, segment] : segs_info) {
            free(mem, segment.addr);
        }
    };

    for (Elf_Half seg_index = 0; seg_index < elf.e_phnum; ++seg_index) {
        const Elf32_Phdr &seg_header = segments[seg_index];
        const uint8_t *const seg_bytes = is_self
            ? (image_bytes + self_header.header_len + seg_header.p_offset)
            : (elf_bytes + seg_header.p_offset);

        const auto uncompress_segment = [&](void *dst) {
            unsigned long dest_bytes = seg_header.p_filesz;
            const uint8_t *const compressed_segment_bytes = image_bytes + seg_infos[seg_index].offset;
            int res = mz_uncompress(static_cast<unsigned char *>(dst), &dest_bytes, compressed_segment_bytes, static_cast<mz_ulong>(seg_infos[seg_index].length));
            assert(res == MZ_OK);
        };

        LOG_DEBUG_IF(LOG_MODULE_LOADING, "    [{}] (p_type: {}): p_offset: {}, p_vaddr: {}, p_paddr: {}, p_filesz: {}, p_memsz: {}, p_flags: {}, p_align: {}", get_seg_header_string(seg_header.p_type), log_hex(seg_header.p_type), log_hex(seg_header.p_offset), log_hex(seg_header.p_vaddr), log_hex(seg_header.p_paddr), log_hex(seg_header.p_filesz), log_hex(seg_header.p_memsz), log_hex(seg_header.p_flags), log_hex(seg_header.p_align));

        if (is_self && seg_infos[seg_index].encryption != 2) { // 0 should also be valid?
            LOG_ERROR("Cannot load ELF {}: invalid segment encryption status {}.", self_path, seg_infos[seg_index].encryption);
            free_all_segments(mem, segment_reloc_info);
            return -1;
        }

        if (seg_header.p_type == PT_NULL) {
            // Nothing to do.
        } else if (seg_header.p_type == PT_LOAD) {
            if (seg_header.p_memsz != 0) {
                Address segment_address = 0;
                auto alloc_name = fmt::format("{}:seg{}", self_path, seg_index);

                segment_address = try_alloc_at(mem, seg_header.p_vaddr, seg_header.p_memsz, alloc_name.c_str());

                if (!segment_address) {
                    if (isRelocatable) { // Try allocating somewhere else
                        segment_address = alloc(mem, seg_header.p_memsz, alloc_name.c_str());
                    }

                    if (!segment_address) {
                        LOG_CRITICAL("Loading {} ELF {} failed: Could not allocate {} bytes @ {} for segment {}.", (isRelocatable) ? "relocatable" : "fixed", self_path, log_hex(seg_header.p_memsz), log_hex(seg_header.p_vaddr), seg_index);
                        free_all_segments(mem, segment_reloc_info);
                        return SCE_KERNEL_ERROR_NO_MEMORY; // TODO is this correct?
                    }
                }

                const Ptr<uint8_t> seg_ptr(segment_address);
                if (is_self && seg_infos[seg_index].compression == 2) {
                    uncompress_segment(seg_ptr.get(mem));
                } else {
                    memcpy(seg_ptr.get(mem), seg_bytes, seg_header.p_filesz);
                }

                segment_reloc_info[seg_index] = { segment_address, seg_header.p_vaddr, seg_header.p_memsz };
            }
        } else if (seg_header.p_type == PT_SCE_RELA) {
            const void *reloc_data = seg_bytes;
            std::unique_ptr<uint8_t[]> uncompressed;

            if (is_self && seg_infos[seg_index].compression == 2) {
                uncompressed = std::make_unique<uint8_t[]>(seg_header.p_filesz);
                uncompress_segment(uncompressed.get());
                reloc_data = uncompressed.get();
            }

            if (!relocate(reloc_data, seg_header.p_filesz, segment_reloc_info, mem)) {
                return -1;
            }
        } else if ((seg_header.p_type == PT_SCE_COMMENT) || (seg_header.p_type == PT_SCE_VERSION)
            || (seg_header.p_type == PT_ARM_EXIDX) /* TODO: this may be important and require being loaded */) {
            LOG_INFO("{}: Skipping special segment {}...", self_path, log_hex(seg_header.p_type));
        } else {
            LOG_CRITICAL("{}: Skipping segment with unknown p_type {}!", self_path, log_hex(seg_header.p_type));
        }
    }

    if (kernel.debugger.dump_elfs || std::getenv("V3K_DUMP_ELFS")) {
        const uint8_t *dump_begin = is_self ? (image_bytes + self_header.header_len) : elf_bytes;
        const uint8_t *dump_end;

        if (is_self) {
            dump_end = image_bytes + self_header.self_filesize;
        } else {
            size_t elf_size = 0;
            auto dump_segments = reinterpret_cast<const Elf32_Phdr *>(elf_bytes + elf.e_phoff);
            for (const auto &[seg_index, segment] : segment_reloc_info) {
                uint8_t *seg_bytes = Ptr<uint8_t>(segment.addr).get(mem);
                elf_size = std::max(elf_size, static_cast<size_t>(dump_segments[seg_index].p_offset) + static_cast<size_t>(dump_segments[seg_index].p_filesz));
            }
            dump_end = elf_bytes + elf_size;
        }

        std::vector<uint8_t> dump_elf(dump_begin, dump_end);
        if (is_self) {
            dump_elf.resize(self_header.elf_filesize);
        }

        Elf32_Phdr *dump_segments = reinterpret_cast<Elf32_Phdr *>(dump_elf.data() + elf.e_phoff);
        uint16_t last_index = 0;
        for (const auto &[seg_index, segment] : segment_reloc_info) {
            uint8_t *seg_bytes = Ptr<uint8_t>(segment.addr).get(mem);
            memcpy(dump_elf.data() + dump_segments[seg_index].p_offset, seg_bytes, dump_segments[seg_index].p_filesz);
            dump_segments[seg_index].p_vaddr = segment.addr;
            last_index = std::max(seg_index, last_index);
        }
        fs::create_directories(dump_path);
        const auto start = dump_segments[0].p_vaddr;
        const auto end = dump_segments[last_index].p_vaddr + dump_segments[last_index].p_filesz;
        const auto elf_name = fs::path(self_path).filename().stem().string();
        const auto filename = dump_path / fmt::format("{}-{}_{}.elf", log_hex_full(start), log_hex_full(end), elf_name);
        fs_utils::dump_data(filename, dump_elf.data(), dump_elf.size());
    }

    const unsigned int module_info_segment_index = elf.e_entry >> 30;
    const Ptr<const uint8_t> module_info_segment_address = Ptr<const uint8_t>(segment_reloc_info[module_info_segment_index].addr);
    const uint8_t *const module_info_segment_bytes = module_info_segment_address.get(mem);
    const sce_module_info_raw *const module_info = reinterpret_cast<const sce_module_info_raw *>(module_info_segment_bytes + module_info_offset);

    for (const auto &[seg, infos] : segment_reloc_info) {
        LOG_INFO("Loaded module segment {} @ [0x{:08X} - 0x{:08X} / 0x{:08X}] (size: 0x{:08X}) of module {}", seg, infos.addr, infos.addr + infos.size, infos.p_vaddr, infos.size, self_path);
    }

    // [NP readiness experiment, opt-in via V3K_NP_READY_PATCH]
    // The Unity NP Toolkit dispatcher FUN_80f4162e fires the C#-side "NP ready"
    // delegate (FUN_80f1f632) only when a type-0/sub-0 event is dispatched AND the
    // NP-manager object's net-state field (+0x74) is non-zero. Running offline with no
    // NP service daemon that readiness path is never reached, so the game never leaves
    // the loading screen (see build-man/NP-RE-FINDINGS.md §8).
    //
    // Experiment A: force the dispatcher to ALWAYS take the readiness path (code 1) for
    // any event it processes. Three 2-byte patches (offsets relative to the dumped ELF
    // seg0 vaddr 0x80F08000; applied at load time before any toolkit code is JIT'd):
    //   0x3964e: beq 0x80f4173e (76 d0) -> b 0x80f4173e (76 e0)  [always type-0 path]
    //   0x39742: bne 0x80f417a4 (2f d1) -> nop          (00 bf)  [don't skip on sub!=0]
    //   0x39774: ldr r1,[r5,#0x74] (69 6f) -> movs r1,#3 (03 21) [force net-state=3]
    if (std::getenv("V3K_NP_READY_PATCH")) {
        const auto seg0_it = segment_reloc_info.find(0);
        if (seg0_it != segment_reloc_info.end() && std::strncmp(module_info->name, "UnityNpToolkit", 28) == 0) {
            const uint32_t seg0 = seg0_it->second.addr;
            struct BytePatch {
                uint32_t off;
                uint8_t expect[2];
                uint8_t patch[2];
                const char *desc;
            };
            // Experiment B (env V3K_NP_READY_PATCH=2): also force the Toolkit::NP worker
            // FUN_80f420a2 to dispatch even when its event queue is empty. The dispatch is
            // gated by `if (piVar10[5]!=0) beq 0x80f4241e` at 0x80f423cc. Redirect that
            // branch to 0x80f4240c (the dispatch-prep that calls FUN_80f4162e with the
            // on-stack buffer), skipping the ring-buffer dequeue so no bad-pointer deref.
            // With Experiment A's patches the dispatcher ignores event content and fires
            // the readiness delegate. Tests whether forcing readiness unblocks C# at all.
            const bool exp_b = std::strcmp(std::getenv("V3K_NP_READY_PATCH"), "2") == 0;
            // readiness code sweep: force net-state field (+0x74) to N via `movs r1,#N`.
            // N=3 -> readiness code 1 (online), N=1 -> code 0, N=2 -> code -1 (0xffffffff).
            uint8_t ns = 3;
            if (const char *e = std::getenv("V3K_NP_READY_NS")) {
                const int v = std::atoi(e);
                if (v >= 1 && v <= 7)
                    ns = static_cast<uint8_t>(v);
            }
            // optionally also force the +0x73 code-2 send (cbz r0 at 0x80f41798 -> nop)
            const bool force_c2 = std::getenv("V3K_NP_READY_C2") != nullptr;
            const BytePatch patches_a[] = {
                { 0x3964e, { 0x76, 0xd0 }, { 0x76, 0xe0 }, "always-type0" },
                { 0x39742, { 0x2f, 0xd1 }, { 0x00, 0xbf }, "ignore-subtype" },
                { 0x39774, { 0x69, 0x6f }, { ns, 0x21 }, "force-netstate" },
            };
            const BytePatch patch_b = { 0x3a3cc, { 0x27, 0xd0 }, { 0x1e, 0xe0 }, "force-dispatch" };
            const BytePatch patch_c2 = { 0x39798, { 0x20, 0xb1 }, { 0x00, 0xbf }, "force-code2" };
            std::vector<BytePatch> patches(std::begin(patches_a), std::end(patches_a));
            if (exp_b)
                patches.push_back(patch_b);
            if (force_c2)
                patches.push_back(patch_c2);
            for (const auto &bp : patches) {
                uint8_t *p = Ptr<uint8_t>(seg0 + bp.off).get(mem);
                if (p[0] == bp.expect[0] && p[1] == bp.expect[1]) {
                    p[0] = bp.patch[0];
                    p[1] = bp.patch[1];
                    LOG_INFO("[NP-READY-PATCH] {} @0x{:08X}: {:02x} {:02x} -> {:02x} {:02x}", bp.desc, seg0 + bp.off, bp.expect[0], bp.expect[1], bp.patch[0], bp.patch[1]);
                } else {
                    LOG_WARN("[NP-READY-PATCH] {} @0x{:08X}: unexpected {:02x} {:02x}; not patching", bp.desc, seg0 + bp.off, p[0], p[1]);
                }
            }
        }
    }

    // [NP message injection — applied unconditionally] (see NP-RE-FINDINGS.md §10/§11)
    // Deliver one synthetic kNPToolKit_NPInitialized (PluginMessage.type=4) so managed
    // Sony.NP.Main.PumpMessages fires OnNPInitialized -> SceneFirst._NpReady=true -> the
    // game leaves the "checking system data" loading screen. The native toolkit never
    // enqueues this message (its init readiness never completes). Patch the toolkit's
    // native message-queue exports in-place (each export is larger than the replacement),
    // using a one-shot flag byte in seg1 BSS so exactly one message is delivered:
    //   PrxHasMessage      (off 0x5EF8): r0 = (flag==0) ? 1 : 0
    //   PrxGetFirstMessage (off 0x5F76, r0=&msg): *(u32*)r0 = 4; flag = 1; return
    //   PrxRemoveFirstMessage (off 0x604A): bx lr (no-op; real queue unused offline)
    // The Sony Unity plugins (UnityNpToolkit, ScreenShots, SavedGames) each export the SAME
    // generic Prx message-queue NIDs (HasMessage 0xB84EBFDC, GetFirstMessage 0x7DF8200B,
    // RemoveFirstMessage 0xBE0F7811). Managed P/Invoke binding may resolve to whichever module
    // registered the NID first (export_nids is first-wins), NOT necessarily the named target.
    // So instrument BOTH UnityNpToolkit and ScreenShots: count calls + deliver a one-shot
    // PluginMessage.type=4 (kNPToolKit_NPInitialized) on GetFirstMessage. The scratch (flag +
    // 2 call counters) lives in the patched function's own dead code tail (seg0, writable).
    // The byte-prologue guard inside patch_prx (push {r4-r6,lr} == 70 b5) makes this a no-op
    // for any module whose offsets don't match this game's Sony Unity plugins, so it is safe
    // to apply for every loaded SELF without an opt-in env var.
    {
        // literal-pool offset for `ldr rX,[pc,#12]` placed at instr_off inside a fn at vaddr base
        auto litoff = [](uint32_t base, uint32_t instr_off) -> uint32_t {
            const uint32_t pc = (base + instr_off + 4) & ~3u;
            return (pc + 12) - base;
        };
        auto patch_prx = [&](uint32_t has_a, uint32_t get_a, uint32_t rem_a, uint32_t scratch, const char *tag) -> bool {
            uint8_t *has = Ptr<uint8_t>(has_a).get(mem);
            uint8_t *get = Ptr<uint8_t>(get_a).get(mem);
            if (!(has[0] == 0x70 && has[1] == 0xb5 && get[0] == 0x70 && get[1] == 0xb5)) {
                LOG_WARN("[NP-MSG-INJECT] {}: unexpected prologue has={:02x}{:02x} get={:02x}{:02x}; skip", tag, has[0], has[1], get[0], get[1]);
                return false;
            }
            memset(Ptr<uint8_t>(scratch).get(mem), 0, 16);
            // HasMessage: r0=&scratch; ++scratch[+4]; return (scratch[0]==0)?1:0
            // ldr r0,[pc,#12]; ldr r1,[r0,#4]; adds r1,#1; str r1,[r0,#4]; ldrb r0,[r0]; movs r1,#1; eors r0,r1; bx lr
            const uint8_t hcode[] = { 0x03, 0x48, 0x41, 0x68, 0x01, 0x31, 0x41, 0x60,
                0x00, 0x78, 0x01, 0x21, 0x48, 0x40, 0x70, 0x47 };
            memcpy(has, hcode, sizeof(hcode));
            memcpy(has + litoff(has_a, 0), &scratch, 4);
            // GetFirstMessage(r0=&msg): msg.type=4; scratch[0]=1; ++scratch[+8]
            // movs r1,#4; str r1,[r0]; ldr r3,[pc,#12]; movs r1,#1; strb r1,[r3]; ldr r1,[r3,#8]; adds r1,#1; str r1,[r3,#8]; bx lr
            const uint8_t gcode[] = { 0x04, 0x21, 0x01, 0x60, 0x03, 0x4b, 0x01, 0x21,
                0x19, 0x70, 0x99, 0x68, 0x01, 0x31, 0x99, 0x60, 0x70, 0x47 };
            memcpy(get, gcode, sizeof(gcode));
            memcpy(get + litoff(get_a, 4), &scratch, 4);
            if (rem_a) {
                uint8_t *rem = Ptr<uint8_t>(rem_a).get(mem);
                rem[0] = 0x70; // bx lr (no-op)
                rem[1] = 0x47;
            }
            LOG_INFO("[NP-MSG-INJECT] {}: patched has=0x{:08X} get=0x{:08X} scratch=0x{:08X}", tag, has_a, get_a, scratch);
            return true;
        };
        const auto s0 = segment_reloc_info.find(0);
        if (s0 != segment_reloc_info.end() && std::strncmp(module_info->name, "ScreenShots", 12) == 0) {
            const uint32_t b = s0->second.addr;
            patch_prx(b + 0x644, b + 0x694 /*real impl behind veneer*/, b + 0x7DC, b + 0x658, "ScreenShots");
        }
        if (s0 != segment_reloc_info.end() && std::strncmp(module_info->name, "UnityNpToolkit", 28) == 0) {
            const uint32_t b = s0->second.addr;
            patch_prx(b + 0x5EF8, b + 0x5F76, b + 0x604A, b + 0x5F0C, "UnityNpToolkit");
        }
    }

    // [Spin trace, opt-in via V3K_SPIN_TRACE] Diagnose the Mono-JIT'd C# atomic spin in
    // Unity_main_thread (see NP-RE-FINDINGS.md §8.3.4): dump its guest PC + registers +
    // the code bytes around PC at intervals, so the LDREX/CAS target address can be found.
    if (std::getenv("V3K_SPIN_TRACE") && std::strncmp(module_info->name, "UnityNpToolkit", 28) == 0) {
        KernelState *kp = &kernel;
        MemState *mp = &mem;
        std::thread([kp, mp]() {
            std::this_thread::sleep_for(std::chrono::seconds(14));
            for (int iter = 0; iter < 6; ++iter) {
                ThreadStatePtr ut;
                {
                    const std::lock_guard<std::mutex> g(kp->mutex);
                    for (const auto &[id, t] : kp->threads) {
                        if (t && t->name == "Unity_main_thread") {
                            ut = t;
                            break;
                        }
                    }
                }
                if (ut && ut->cpu) {
                    CPUState &c = *ut->cpu;
                    const uint32_t pc = read_pc(c);
                    std::string regs;
                    for (int r = 0; r <= 12; ++r)
                        regs += fmt::format("r{}=0x{:08X} ", r, read_reg(c, r));
                    LOG_INFO("[SPIN-TRACE {}] pc=0x{:08X} sp=0x{:08X} lr=0x{:08X} {}", iter, pc, read_sp(c), read_lr(c), regs);
                    const uint32_t base = (pc & ~1u) - 24;
                    std::string code;
                    for (uint32_t a = base; a < base + 56; a += 2) {
                        const Ptr<uint16_t> hw(a);
                        if (hw && hw.valid(*mp))
                            code += fmt::format("{:04x}@{:08X} ", *hw.get(*mp), a);
                    }
                    LOG_INFO("[SPIN-TRACE {}] code: {}", iter, code);
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            LOG_INFO("[SPIN-TRACE] done");
        }).detach();
    }

    const SceKernelModulePtr kernelModuleInfo = std::make_shared<KernelModule>();
    memset(kernelModuleInfo.get(), 0, sizeof(KernelModule));

    kernelModuleInfo->info_segment_address = module_info_segment_address;
    kernelModuleInfo->info_offset = module_info_offset;

    auto *sceKernelModuleInfo = &kernelModuleInfo->info;
    sceKernelModuleInfo->size = sizeof(*sceKernelModuleInfo);
    strncpy(sceKernelModuleInfo->module_name, module_info->name, 28);
    // unk28
    if (module_info->module_start != 0xffffffff && module_info->module_start != 0)
        sceKernelModuleInfo->start_entry = module_info_segment_address + module_info->module_start;
    // unk30
    if (module_info->module_stop != 0xffffffff && module_info->module_stop != 0)
        sceKernelModuleInfo->stop_entry = module_info_segment_address + module_info->module_stop;

    sceKernelModuleInfo->exidx_top = Ptr<const void>(module_info->exidx_top);
    sceKernelModuleInfo->exidx_btm = Ptr<const void>(module_info->exidx_end);
    sceKernelModuleInfo->extab_top = Ptr<const void>(module_info->extab_top);
    sceKernelModuleInfo->extab_btm = Ptr<const void>(module_info->extab_end);

    sceKernelModuleInfo->tlsInit = Ptr<const void>(!module_info->tls_start ? 0 : (module_info_segment_address.address() + module_info->tls_start));
    sceKernelModuleInfo->tlsInitSize = module_info->tls_filesz;
    sceKernelModuleInfo->tlsAreaSize = module_info->tls_memsz;

    if (sceKernelModuleInfo->tlsInit) {
        kernel.tls_address = sceKernelModuleInfo->tlsInit;
        kernel.tls_psize = sceKernelModuleInfo->tlsInitSize;
        kernel.tls_msize = sceKernelModuleInfo->tlsAreaSize;
    }

    strncpy(sceKernelModuleInfo->path, self_path.c_str(), 255);

    for (Elf_Half segment_index = 0; segment_index < elf.e_phnum; ++segment_index) {
        // Skip non-loadable segments
        auto it = segment_reloc_info.find(segment_index);
        if (it == segment_reloc_info.end())
            continue;

        if (segment_index >= MODULE_INFO_NUM_SEGMENTS) {
            LOG_ERROR("Segment {} should not be loadable", segment_index);
            continue;
        }

        SceKernelSegmentInfo &segment = sceKernelModuleInfo->segments[segment_index];
        segment.size = sizeof(segment);
        segment.vaddr = it->second.addr;
        segment.memsz = segments[segment_index].p_memsz;
        segment.filesz = segments[segment_index].p_filesz;
    }

    sceKernelModuleInfo->state = module_info->type;

    LOG_INFO("Linking {} {}...", is_self ? "SELF" : "ELF", self_path);
    if (self_path.contains("eboot.bin"))
        LOG_INFO("eboot.bin module NID: {}", log_hex(module_info->module_nid));

    if (!load_exports(sceKernelModuleInfo, *module_info, module_info_segment_address, kernel, mem)) {
        return -1;
    }

    if (!load_imports(*module_info, module_info_segment_address, segment_reloc_info, kernel, mem)) {
        return -1;
    }
    const SceUID uid = kernel.get_next_uid();
    sceKernelModuleInfo->modid = uid;
    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        kernel.loaded_modules[uid] = kernelModuleInfo;
    }
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        kernel.module_uid_by_nid[module_info->module_nid] = uid;
    }

    return uid;
}

int unload_self(KernelState &kernel, MemState &mem, KernelModule &module) {
    LOG_INFO("Unlinking self...");
    const sce_module_info_raw *const module_info = reinterpret_cast<const sce_module_info_raw *>(module.info_segment_address.get(mem) + module.info_offset);
    if (!load_exports(&module.info, *module_info, module.info_segment_address, kernel, mem, true)) {
        return -1;
    }

    SegmentInfosForReloc segment_reloc_info;
    for (int i = 0; i < MODULE_INFO_NUM_SEGMENTS; i++) {
        const auto &segment = module.info.segments[i];
        if (segment.size == 0)
            continue;

        segment_reloc_info[i] = { segment.vaddr.address(), 0, segment.memsz }; // p_vaddr is not used in variable relocations
    }

    if (!load_imports(*module_info, module.info_segment_address, segment_reloc_info, kernel, mem, true)) {
        return -1;
    }

    SceUID mod_nid = module_info->module_nid;

    // last step: free the memory
    for (int i = 0; i < MODULE_INFO_NUM_SEGMENTS; i++) {
        const auto &segment = module.info.segments[i];
        if (segment.size == 0)
            continue;

        kernel.invalidate_jit_cache(segment.vaddr.address(), segment.memsz);
        free(mem, module.info.segments[i].vaddr.address());
    }

    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        kernel.loaded_modules.erase(module.info.modid);
    }
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        kernel.module_uid_by_nid.erase(mod_nid);
    }

    return 0;
}
