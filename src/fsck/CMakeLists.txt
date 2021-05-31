    aktualizr_source_file_checks(
            fsck.cc)
if(BUILD_OSTREE)


    add_executable(fsck fsck.cc)

    target_link_libraries(fsck
            sota_tools_lib
            ${LIBOSTREE_LIBRARIES}
            Boost::boost Threads::Threads)

endif(BUILD_OSTREE)
