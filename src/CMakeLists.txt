project(address-book-service)

set(CONTACTS_SERVICE_BIN address-book-service)

set(CONTACTS_SERVICE_BIN_SRC
    main.cpp
)

add_executable(${CONTACTS_SERVICE_BIN}
    ${CONTACTS_SERVICE_BIN_SRC}
)

target_link_libraries(${CONTACTS_SERVICE_BIN}
    address-book-service-lib
    ${GLIB_LIBRARIES}
    ${GIO_LIBRARIES}
    ${FOLKS_LIBRARIES}
    Qt5::Core
    Qt5::Contacts
)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
    ${address-book-service-lib_SOURCE_DIR}
    ${GLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${FOLKS_INCLUDE_DIRS}
)

install(TARGETS ${CONTACTS_SERVICE_BIN}
        RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_LIBEXECDIR})
