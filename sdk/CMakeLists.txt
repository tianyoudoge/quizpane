add_library(quizpane_provider_sdk INTERFACE)
target_include_directories(quizpane_provider_sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(quizpane_provider_host STATIC
    include/quizpane/provider_installer.hpp
    include/quizpane/provider_loader.hpp
    src/provider_installer.cpp
    src/provider_loader.cpp
)
target_include_directories(quizpane_provider_host PUBLIC include)
target_link_libraries(quizpane_provider_host
    PUBLIC quizpane_provider_sdk Qt6::Core
    PRIVATE Qt6::CorePrivate
)
if(APPLE)
    target_link_libraries(quizpane_provider_host PUBLIC
        "-framework Security" "-framework CoreFoundation")
endif()
if(WIN32)
    target_link_libraries(quizpane_provider_host PUBLIC Advapi32)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LibSecret QUIET IMPORTED_TARGET libsecret-1)
        if(TARGET PkgConfig::LibSecret)
            target_compile_definitions(quizpane_provider_host PRIVATE
                QUIZPANE_HAVE_LIBSECRET=1)
            target_link_libraries(quizpane_provider_host PRIVATE
                PkgConfig::LibSecret)
        endif()
    endif()
endif()
