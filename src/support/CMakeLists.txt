file(GLOB SOURCES *.h *.cpp)
add_library(deltaSupport ${SOURCES})

llvm_map_components_to_libnames(LLVM_LIBS support)
target_link_libraries(deltaSupport ${LLVM_LIBS})
