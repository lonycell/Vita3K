set(TRANSLATOR_SOURCES
	include/translator/types.h
	include/translator/ocr.h
	include/translator/net.h
	include/translator/state.h
	src/translator.cpp
)

if(APPLE)
	list(APPEND TRANSLATOR_SOURCES
		src/ocr_vision.mm
		src/net_deepl.mm)
else()
	list(APPEND TRANSLATOR_SOURCES src/backends_stub.cpp)
endif()

add_library(translator STATIC ${TRANSLATOR_SOURCES})

target_include_directories(translator PUBLIC include)
target_link_libraries(translator PUBLIC util)
target_link_libraries(translator PRIVATE fmt)

if(APPLE)
	target_link_libraries(translator PRIVATE
		"-framework Foundation"
		"-framework CoreGraphics"
		"-framework Vision")
	set_source_files_properties(src/ocr_vision.mm src/net_deepl.mm
		PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()
