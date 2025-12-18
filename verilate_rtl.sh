#!/bin/bash
#=============================================================================
# verilate_rtl.sh - RTL을 SystemC로 변환하는 스크립트
#=============================================================================

set -e  # 에러 발생 시 중단

#=============================================================================
# 1. 환경 변수 설정
#=============================================================================

# RTL 경로 설정 (필요에 따라 수정)
export IP_RTL_PATH="/path/to/your/ip/rtl"
# export IP_RTL_PATH="$HOME/project/rtl"
# export IP_RTL_PATH="$(pwd)/../rtl"

# 추가 RTL 경로가 있다면
# export COMMON_RTL_PATH="/path/to/common/rtl"
# export PERIPH_RTL_PATH="/path/to/peripheral/rtl"

# Verilator & SystemC 환경 로드
source ~/local/eda/setup_env.sh

#=============================================================================
# 2. 설정 변수
#=============================================================================

TOP_MODULE="top"                    # 탑 모듈 이름
RTL_LIST="rtl_files.f"              # RTL 파일 리스트
TB_FILE="tb_top.cpp"                # 테스트벤치 (없으면 빈 문자열)
OUT_DIR="obj_dir"                   # 출력 디렉토리
THREADS=$(nproc)                    # 병렬 스레드 수

#=============================================================================
# 3. 환경 변수 확인
#=============================================================================

echo "=== 환경 변수 확인 ==="
echo "IP_RTL_PATH:    $IP_RTL_PATH"
echo "SYSTEMC_HOME:   $SYSTEMC_HOME"
echo "VERILATOR_ROOT: $VERILATOR_ROOT"
echo "CXX_STANDARD:   $CXX_STANDARD"
echo ""

# IP_RTL_PATH 존재 확인
if [ ! -d "$IP_RTL_PATH" ]; then
    echo "ERROR: IP_RTL_PATH 디렉토리가 존재하지 않습니다: $IP_RTL_PATH"
    exit 1
fi

# rtl_files.f 존재 확인
if [ ! -f "$RTL_LIST" ]; then
    echo "ERROR: RTL 파일 리스트가 존재하지 않습니다: $RTL_LIST"
    exit 1
fi

#=============================================================================
# 4. 파일 리스트 내용 확인 (디버깅용)
#=============================================================================

echo "=== RTL 파일 리스트 (환경 변수 확장 후) ==="
# 환경 변수가 제대로 확장되는지 확인
envsubst < "$RTL_LIST" | head -20
echo "..."
echo ""

#=============================================================================
# 5. 기존 출력 정리
#=============================================================================

echo "=== 기존 빌드 정리 ==="
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

#=============================================================================
# 6. Verilator 실행
#=============================================================================

echo "=== Verilator 변환 시작 ==="

VERILATOR_OPTS=(
    --sc
    --top-module "$TOP_MODULE"
    --threads "$THREADS"
    --trace-fst
    -O3
    --x-assign 0
    --x-initial 0
    --x-initial-edge
    --unroll-count 256
    --output-split 20000
    --output-split-cfuncs 20000
    --no-timing
    --pins-sc-uint-bool
    -Wno-fatal
    -Wno-WIDTHEXPAND
    -Wno-WIDTHTRUNC
    -Wno-UNUSED
    -CFLAGS "-std=c++$CXX_STANDARD -O3 -march=native -fPIC -I$SYSTEMC_INCLUDE"
    -LDFLAGS "-L$SYSTEMC_LIBDIR -lsystemc -Wl,-rpath,$SYSTEMC_LIBDIR"
    --Mdir "$OUT_DIR"
    -f "$RTL_LIST"
)

# 테스트벤치가 있으면 추가
if [ -n "$TB_FILE" ] && [ -f "$TB_FILE" ]; then
    VERILATOR_OPTS+=(--exe "$TB_FILE" -o "V$TOP_MODULE")
fi

# 실행
verilator "${VERILATOR_OPTS[@]}"

echo ""
echo "=== Verilator 변환 완료 ==="

#=============================================================================
# 7. C++ 빌드
#=============================================================================

echo "=== C++ 빌드 시작 ==="

make -C "$OUT_DIR" -f "V${TOP_MODULE}.mk" -j"$THREADS"

echo ""
echo "=== 빌드 완료 ==="

#=============================================================================
# 8. 완료 메시지
#=============================================================================

echo ""
echo "========================================"
echo "변환 및 빌드 완료!"
echo "========================================"
echo ""
echo "출력 위치: $OUT_DIR/"
echo ""

if [ -f "$OUT_DIR/V$TOP_MODULE" ]; then
    echo "실행 방법:"
    echo "  ./$OUT_DIR/V$TOP_MODULE"
else
    echo "라이브러리 생성됨:"
    ls -la "$OUT_DIR/"*.a 2>/dev/null || echo "  (정적 라이브러리 없음)"
fi
echo ""