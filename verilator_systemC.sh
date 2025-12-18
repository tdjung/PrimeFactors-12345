#!/bin/bash

#=============================================================================
# 설정: 원하는 설치 경로 지정
#=============================================================================
INSTALL_BASE="$HOME/local/eda"  # 원하는 경로로 변경
SYSTEMC_VERSION="2.3.4"
VERILATOR_VERSION="5.028"       # 최신 안정 버전

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
# 1. SystemC 설치
#=============================================================================
echo ""
echo "=== SystemC $SYSTEMC_VERSION 설치 시작 ==="
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
mkdir -p build && cd build

# CMake 설정 및 빌드
cmake .. \
    -DCMAKE_INSTALL_PREFIX="$SYSTEMC_INSTALL" \
    -DCMAKE_CXX_STANDARD=17 \
    -DBUILD_SHARED_LIBS=ON \
    -DENABLE_ASSERTIONS=ON

make -j$NPROC
make install

echo "=== SystemC 설치 완료: $SYSTEMC_INSTALL ==="

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
cat > "$ENV_FILE" << 'ENVEOF'
#!/bin/bash
# Verilator & SystemC 환경 설정

INSTALL_BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# SystemC
export SYSTEMC_HOME="$INSTALL_BASE/systemc-SYSTEMC_VERSION_PLACEHOLDER"
export SYSTEMC_INCLUDE="$SYSTEMC_HOME/include"
export SYSTEMC_LIBDIR="$SYSTEMC_HOME/lib"
export LD_LIBRARY_PATH="$SYSTEMC_HOME/lib:$LD_LIBRARY_PATH"

# Verilator
export VERILATOR_ROOT="$INSTALL_BASE/verilator-VERILATOR_VERSION_PLACEHOLDER"
export PATH="$VERILATOR_ROOT/bin:$PATH"

# PKG_CONFIG (CMake용)
export PKG_CONFIG_PATH="$SYSTEMC_HOME/lib/pkgconfig:$PKG_CONFIG_PATH"

echo "Environment configured:"
echo "  SYSTEMC_HOME=$SYSTEMC_HOME"
echo "  VERILATOR_ROOT=$VERILATOR_ROOT"
ENVEOF

# 버전 정보 치환
sed -i "s/SYSTEMC_VERSION_PLACEHOLDER/$SYSTEMC_VERSION/g" "$ENV_FILE"
sed -i "s/VERILATOR_VERSION_PLACEHOLDER/$VERILATOR_VERSION/g" "$ENV_FILE"

chmod +x "$ENV_FILE"

#=============================================================================
# 완료 메시지
#=============================================================================
echo ""
echo "========================================"
echo "설치 완료!"
echo "========================================"
echo ""
echo "사용 방법:"
echo "  source $ENV_FILE"
echo ""
echo "설치 경로:"
echo "  SystemC:   $SYSTEMC_INSTALL"
echo "  Verilator: $VERILATOR_INSTALL"
echo ""
echo "테스트:"
echo "  verilator --version"
echo ""