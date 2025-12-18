#!/bin/bash

#=============================================================================
# 설정: 원하는 설치 경로 지정
#=============================================================================
INSTALL_BASE="$HOME/local/eda"  # 원하는 경로로 변경
SYSTEMC_VERSION="2.3.4"
VERILATOR_VERSION="5.028"
CXX_STANDARD=17                 # C++ 버전 통일 (중요!)

# 디렉토리 구조
SYSTEMC_INSTALL="$INSTALL_BASE/systemc-$SYSTEMC_VERSION"
VERILATOR_INSTALL="$INSTALL_BASE/verilator-$VERILATOR_VERSION"
SRC_DIR="$INSTALL_BASE/src"

# 병렬 빌드 스레드 수
NPROC=$(nproc)

#=============================================================================
# 의존성 확인 (Ubuntu/Debian 기준)
#=============================================================================
echo "=== 의존성 패키지 확인 ==="
# 필요시 설치: sudo apt install build-essential autoconf flex bison git help2man perl python3 libfl2 libfl-dev zlib1g-dev

#=============================================================================
# 디렉토리 생성
#=============================================================================
mkdir -p "$SRC_DIR"
mkdir -p "$SYSTEMC_INSTALL"
mkdir -p "$VERILATOR_INSTALL"

#=============================================================================
# 1. SystemC 설치 (C++17)
#=============================================================================
echo ""
echo "=== SystemC $SYSTEMC_VERSION 설치 시작 (C++$CXX_STANDARD) ==="
cd "$SRC_DIR"

# 소스 다운로드
if [ ! -f "systemc-$SYSTEMC_VERSION.tar.gz" ]; then
    wget https://github.com/accellera-official/systemc/archive/refs/tags/$SYSTEMC_VERSION.tar.gz \
        -O systemc-$SYSTEMC_VERSION.tar.gz
fi

# 압축 해제
rm -rf systemc-$SYSTEMC_VERSION
tar -xzf systemc-$SYSTEMC_VERSION.tar.gz
cd systemc-$SYSTEMC_VERSION

# 빌드 디렉토리
rm -rf build && mkdir -p build && cd build

# CMake 설정 및 빌드 - C++17 명시
cmake .. \
    -DCMAKE_INSTALL_PREFIX="$SYSTEMC_INSTALL" \
    -DCMAKE_CXX_STANDARD=$CXX_STANDARD \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DENABLE_ASSERTIONS=ON

make -j$NPROC
make install

echo "=== SystemC 설치 완료: $SYSTEMC_INSTALL ==="

# SystemC 빌드 버전 확인
echo "=== SystemC ABI 버전 확인 ==="
nm -C "$SYSTEMC_INSTALL/lib/libsystemc.so" | grep sc_api_version | head -1

#=============================================================================
# 2. Verilator 설치
#=============================================================================
echo ""
echo "=== Verilator $VERILATOR_VERSION 설치 시작 ==="
cd "$SRC_DIR"

# 소스 다운로드 (Git 사용)
if [ ! -d "verilator" ]; then
    git clone https://github.com/verilator/verilator.git
fi

cd verilator
git fetch --all --tags
git checkout v$VERILATOR_VERSION

# Autoconf 실행
autoconf

# 설정 - SystemC 경로 지정
./configure \
    --prefix="$VERILATOR_INSTALL" \
    SYSTEMC_INCLUDE="$SYSTEMC_INSTALL/include" \
    SYSTEMC_LIBDIR="$SYSTEMC_INSTALL/lib"

# 빌드 및 설치
make -j$NPROC
make install

echo "=== Verilator 설치 완료: $VERILATOR_INSTALL ==="

#=============================================================================
# 3. 환경 설정 파일 생성
#=============================================================================
ENV_FILE="$INSTALL_BASE/setup_env.sh"
cat > "$ENV_FILE" << ENVEOF
#!/bin/bash
# Verilator & SystemC 환경 설정

INSTALL_BASE="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"

# C++ 표준 버전 (SystemC 빌드와 동일하게 유지 - 중요!)
export CXX_STANDARD=$CXX_STANDARD

# SystemC
export SYSTEMC_HOME="\$INSTALL_BASE/systemc-$SYSTEMC_VERSION"
export SYSTEMC_INCLUDE="\$SYSTEMC_HOME/include"
export SYSTEMC_LIBDIR="\$SYSTEMC_HOME/lib"
export LD_LIBRARY_PATH="\$SYSTEMC_HOME/lib:\$LD_LIBRARY_PATH"

# Verilator
export VERILATOR_PATH="\$INSTALL_BASE/verilator-$VERILATOR_VERSION"
export PATH="\$VERILATOR_PATH/bin:\$PATH"

# PKG_CONFIG (CMake용)
export PKG_CONFIG_PATH="\$SYSTEMC_HOME/lib/pkgconfig:\$PKG_CONFIG_PATH"

# Verilator SystemC 빌드용 플래그
export VERILATOR_SC_CFLAGS="-std=c++\$CXX_STANDARD -I\$SYSTEMC_INCLUDE"
export VERILATOR_SC_LDFLAGS="-L\$SYSTEMC_LIBDIR -lsystemc -Wl,-rpath,\$SYSTEMC_LIBDIR"

# 편의용 alias
alias verilator_sc='verilator --sc -CFLAGS "\$VERILATOR_SC_CFLAGS" -LDFLAGS "\$VERILATOR_SC_LDFLAGS"'

echo "Environment configured:"
echo "  SYSTEMC_HOME=\$SYSTEMC_HOME"
echo "  VERILATOR_PATH=\$VERILATOR_PATH"
echo "  CXX_STANDARD=c++\$CXX_STANDARD"
ENVEOF

chmod +x "$ENV_FILE"

#=============================================================================
# 4. 테스트 실행
#=============================================================================
echo ""
echo "=== 설치 테스트 ==="

# 환경 로드
source "$ENV_FILE"

# 테스트 디렉토리
TEST_DIR="$INSTALL_BASE/test_install"
mkdir -p "$TEST_DIR" && cd "$TEST_DIR"

# 간단한 Verilog 모듈
cat > counter.v << 'VEOF'
module counter (
    input  wire        clk,
    input  wire        rst_n,
    output reg  [7:0]  count
);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            count <= 8'h0;
        else
            count <= count + 1;
    end
endmodule
VEOF

# SystemC 테스트벤치
cat > tb_counter.cpp << 'CPPEOF'
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
CPPEOF

# Verilator로 변환 및 빌드 (C++ 버전 명시)
rm -rf obj_dir
verilator --sc --exe --trace \
    -CFLAGS "-std=c++$CXX_STANDARD -I$SYSTEMC_INCLUDE" \
    -LDFLAGS "-L$SYSTEMC_LIBDIR -lsystemc -Wl,-rpath,$SYSTEMC_LIBDIR" \
    counter.v tb_counter.cpp \
    -o sim_counter

# 빌드
make -C obj_dir -f Vcounter.mk

# 실행
echo ""
echo "=== 시뮬레이션 실행 ==="
./obj_dir/sim_counter

#=============================================================================
# 완료 메시지
#=============================================================================
echo ""
echo "========================================"
echo "설치 및 테스트 완료!"
echo "========================================"
echo ""
echo "사용 방법:"
echo "  source $ENV_FILE"
echo ""
echo "설치 경로:"
echo "  SystemC:   $SYSTEMC_INSTALL (C++$CXX_STANDARD)"
echo "  Verilator: $VERILATOR_INSTALL"
echo ""
echo "간편 사용법:"
echo "  verilator_sc --exe --trace your_rtl.v tb.cpp"
echo ""
echo "수동 사용법:"
echo "  verilator --sc --exe --trace \\"
echo "      -CFLAGS \"-std=c++$CXX_STANDARD -I\$SYSTEMC_INCLUDE\" \\"
echo "      -LDFLAGS \"-L\$SYSTEMC_LIBDIR -lsystemc -Wl,-rpath,\$SYSTEMC_LIBDIR\" \\"
echo "      your_rtl.v tb.cpp"
echo ""