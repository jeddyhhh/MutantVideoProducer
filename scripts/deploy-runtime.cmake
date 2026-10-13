# Install preserves the source timestamps and reuses unchanged files. Unlike
# copy_directory's byte comparison, repeated links need not reread large model
# and GPU-library files. A clean runtime directory still receives every file.
if(NOT IS_DIRECTORY "${SOURCE}")
    message(FATAL_ERROR "Runtime source directory is missing: ${SOURCE}")
endif()
file(INSTALL DESTINATION "${DESTINATION}" TYPE DIRECTORY FILES "${SOURCE}/"
     USE_SOURCE_PERMISSIONS MESSAGE_NEVER)
