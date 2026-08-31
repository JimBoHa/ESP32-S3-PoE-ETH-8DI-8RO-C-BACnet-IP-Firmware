set(BACNET_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/bacnet-stack")
set(BACNET_SRC "${BACNET_ROOT}/src")

set(BACNET_SOURCES
    "${BACNET_SRC}/bacnet/basic/binding/address.c"
    "${BACNET_SRC}/bacnet/basic/server/bacnet_device.c"
    "${BACNET_SRC}/bacnet/basic/object/ai.c"
    "${BACNET_SRC}/bacnet/basic/object/bi.c"
    "${BACNET_SRC}/bacnet/basic/object/bo.c"
    "${BACNET_SRC}/bacnet/basic/object/netport.c"
    "${BACNET_SRC}/bacnet/basic/npdu/h_npdu.c"
    "${BACNET_SRC}/bacnet/basic/sys/datetime_mstimer.c"
    "${BACNET_SRC}/bacnet/basic/service/h_apdu.c"
    "${BACNET_SRC}/bacnet/basic/service/h_cov.c"
    "${BACNET_SRC}/bacnet/basic/service/h_noserv.c"
    "${BACNET_SRC}/bacnet/basic/service/h_rd.c"
    "${BACNET_SRC}/bacnet/basic/service/h_rp.c"
    "${BACNET_SRC}/bacnet/basic/service/h_rpm.c"
    "${BACNET_SRC}/bacnet/basic/service/h_whohas.c"
    "${BACNET_SRC}/bacnet/basic/service/h_whois.c"
    "${BACNET_SRC}/bacnet/basic/service/h_wp.c"
    "${BACNET_SRC}/bacnet/basic/service/s_iam.c"
    "${BACNET_SRC}/bacnet/basic/service/s_ihave.c"
    "${BACNET_SRC}/bacnet/basic/sys/days.c"
    "${BACNET_SRC}/bacnet/basic/sys/dst.c"
    "${BACNET_SRC}/bacnet/basic/sys/debug.c"
    "${BACNET_SRC}/bacnet/basic/sys/fifo.c"
    "${BACNET_SRC}/bacnet/basic/sys/keylist.c"
    "${BACNET_SRC}/bacnet/basic/sys/mstimer.c"
    "${BACNET_SRC}/bacnet/basic/sys/ringbuf.c"
    "${BACNET_SRC}/bacnet/basic/tsm/tsm.c"
    "${BACNET_SRC}/bacnet/abort.c"
    "${BACNET_SRC}/bacnet/bacaction.c"
    "${BACNET_SRC}/bacnet/bacaddr.c"
    "${BACNET_SRC}/bacnet/bacapp.c"
    "${BACNET_SRC}/bacnet/bacdcode.c"
    "${BACNET_SRC}/bacnet/bacdest.c"
    "${BACNET_SRC}/bacnet/bacdevobjpropref.c"
    "${BACNET_SRC}/bacnet/bacerror.c"
    "${BACNET_SRC}/bacnet/bacint.c"
    "${BACNET_SRC}/bacnet/bacreal.c"
    "${BACNET_SRC}/bacnet/bacstr.c"
    "${BACNET_SRC}/bacnet/cov.c"
    "${BACNET_SRC}/bacnet/datetime.c"
    "${BACNET_SRC}/bacnet/dcc.c"
    "${BACNET_SRC}/bacnet/datalink/bvlc.c"
    "${BACNET_SRC}/bacnet/datalink/bvlc6.c"
    "${BACNET_SRC}/bacnet/hostnport.c"
    "${BACNET_SRC}/bacnet/iam.c"
    "${BACNET_SRC}/bacnet/ihave.c"
    "${BACNET_SRC}/bacnet/indtext.c"
    "${BACNET_SRC}/bacnet/memcopy.c"
    "${BACNET_SRC}/bacnet/npdu.c"
    "${BACNET_SRC}/bacnet/proplist.c"
    "${BACNET_SRC}/bacnet/rd.c"
    "${BACNET_SRC}/bacnet/reject.c"
    "${BACNET_SRC}/bacnet/rp.c"
    "${BACNET_SRC}/bacnet/rpm.c"
    "${BACNET_SRC}/bacnet/timestamp.c"
    "${BACNET_SRC}/bacnet/whohas.c"
    "${BACNET_SRC}/bacnet/whois.c"
    "${BACNET_SRC}/bacnet/wp.c"
)

idf_component_register(
    SRCS ${BACNET_SOURCES}
    INCLUDE_DIRS "include" "${BACNET_SRC}"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BACDL_BIP=1
    BACNET_STACK_DEPRECATED_DISABLE=1
    BACAPP_MINIMAL=1
    BACAPP_HOST_N_PORT=1
    BACNET_PROTOCOL_REVISION=28
    CONFIG_BACNET_BASIC_OBJECT_ANALOG_INPUT=1
    CONFIG_BACNET_BASIC_OBJECT_BINARY_INPUT=1
    CONFIG_BACNET_BASIC_OBJECT_BINARY_OUTPUT=1
    CONFIG_BACNET_BASIC_COV_SUBSCRIPTIONS_SIZE=16
    MAX_COV_SUBSCRIPTIONS=16
    MAX_COV_ADDRESSES=8
    MAX_TSM_TRANSACTIONS=8
    PRINT_ENABLED=0
    BBMD_ENABLED=0
)

# Upstream is warning-clean in its own build, but ESP-IDF enables a different
# warning profile. Keep application warnings strict without treating vendored
# dependency warnings as firmware errors.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-unused-function)
