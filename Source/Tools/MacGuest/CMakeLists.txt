add_library(MacGuest STATIC GuestThread.cpp)
target_include_directories(MacGuest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(MacGuest PUBLIC FEXCore_Base)
