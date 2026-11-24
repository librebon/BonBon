# Included by CPack at package generation time, before CPackDeb looks up
# dpkg-shlibdeps. Point it at the wrapper that injects debian/shlibs.local.
set(SHLIBDEPS_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/cpack-dpkg-shlibdeps.sh")
