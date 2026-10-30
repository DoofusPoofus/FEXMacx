add_executable(FEXMacx FEXMacx.cpp MacGuestFaults.cpp MachOImage.cpp MacSignals.cpp MacSyscalls.cpp MacThreads.cpp MacTime.cpp)
target_include_directories(FEXMacx PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(FEXMacx PRIVATE FEXCore Common CommonTools MacGuest JemallocLibs)
