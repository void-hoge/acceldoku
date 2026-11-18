acceldoku_iverilog(axi tb_acceldoku_axi OPTIONS -Ptb_acceldoku_axi.CORES=${ACCELDOKU_CORES}
    SOURCES ${ACCELDOKU_AXI_RTL} "${CMAKE_CURRENT_SOURCE_DIR}/tb_acceldoku_axi.sv")
