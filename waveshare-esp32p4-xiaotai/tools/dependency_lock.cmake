function(xiaotai_prepare_dependency_lock project_dir build_dir)
    file(TO_CMAKE_PATH "${project_dir}" XIAOTAI_P4_PROJECT_DIR)
    if(EXISTS "${project_dir}/dependencies.lock")
        configure_file("${project_dir}/dependencies.lock"
                       "${build_dir}/dependencies.lock" @ONLY)
    endif()
endfunction()
