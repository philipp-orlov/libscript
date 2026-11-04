add_executable(script_cli script_tool.cpp)
set_target_properties(script_cli PROPERTIES OUTPUT_NAME libscript)
target_link_libraries(script_cli PRIVATE script)
script_set_warnings(script_cli)
install(TARGETS script_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The engine is static: leave out what this tool does not reach.
if(NOT MSVC AND NOT APPLE)
    target_link_options(script_cli PRIVATE -Wl,--gc-sections)
endif()
