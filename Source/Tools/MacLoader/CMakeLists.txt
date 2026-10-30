add_executable(FEXMacx FEXMacx.cpp MachOImage.cpp MacSyscalls.cpp)
target_include_directories(FEXMacx PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(FEXMacx PRIVATE FEXCore Common CommonTools MacGuest JemallocLibs)
