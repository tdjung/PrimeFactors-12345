#include <systemc.h>
#include "Vcounter.h"
#include "verilated.h"
#include "verilated_vcd_sc.h"

int sc_main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    
    // 클럭 생성
    sc_clock clk("clk", 10, SC_NS);
    sc_signal<bool> rst_n;
    sc_signal<uint32_t> count;
    
    // DUT 인스턴스
    Vcounter* dut = new Vcounter("dut");
    dut->clk(clk);
    dut->rst_n(rst_n);
    dut->count(count);
    
    // VCD 트레이스
    VerilatedVcdSc* tfp = new VerilatedVcdSc;
    dut->trace(tfp, 99);
    tfp->open("counter.vcd");
    
    // 시뮬레이션
    rst_n = 0;
    sc_start(20, SC_NS);
    
    rst_n = 1;
    sc_start(200, SC_NS);
    
    // 결과 출력
    std::cout << "Final count: " << count.read() << std::endl;
    
    tfp->close();
    delete dut;
    return 0;
}