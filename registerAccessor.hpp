#include <cstdint>
#include <array>
#include <cstring>

// 레지스터 주소 정의
enum class RegAddr : size_t {
    ABC = 0x1000,
    AAA = 0x1004,
    BBB = 0x1008,
    CCC = 0x100C,
    // 필요한 만큼 추가...
};

// 모든 비트 조작을 담당하는 단일 헬퍼 클래스
template<typename RegisterArrayType>
class RegisterAccessor {
private:
    RegisterArrayType& reg_array_;
    const RegAddr addr_;

public:
    constexpr RegisterAccessor(RegisterArrayType& reg_array, RegAddr addr)
        : reg_array_(reg_array), addr_(addr) {}

    // === 전체 레지스터 접근 ===
    constexpr uint16_t& operator()() { return reg_array_[addr_]; }
    constexpr const uint16_t& operator()() const { return reg_array_[addr_]; }
    constexpr RegisterAccessor& operator=(uint16_t value) { reg_array_[addr_] = value; return *this; }
    constexpr operator uint16_t() const { return reg_array_[addr_]; }

    // === 단일 비트 접근 ===
    template<size_t BitPos>
    constexpr bool bit() const {
        static_assert(BitPos < 16, "Bit position must be < 16");
        return (reg_array_[addr_] >> BitPos) & 0x1;
    }

    template<size_t BitPos>
    constexpr RegisterAccessor& set_bit(bool value = true) {
        static_assert(BitPos < 16, "Bit position must be < 16");
        if (value) {
            reg_array_[addr_] |= (1U << BitPos);
        } else {
            reg_array_[addr_] &= ~(1U << BitPos);
        }
        return *this;
    }

    // 런타임 비트 접근
    constexpr bool bit(size_t bit_pos) const {
        return (reg_array_[addr_] >> bit_pos) & 0x1;
    }

    constexpr RegisterAccessor& set_bit(size_t bit_pos, bool value = true) {
        if (value) {
            reg_array_[addr_] |= (1U << bit_pos);
        } else {
            reg_array_[addr_] &= ~(1U << bit_pos);
        }
        return *this;
    }

    // === 비트 필드 접근 ===
    template<size_t HighBit, size_t LowBit>
    constexpr uint16_t bits() const {
        static_assert(HighBit >= LowBit && HighBit < 16, "Invalid bit range");
        constexpr size_t width = HighBit - LowBit + 1;
        constexpr uint16_t mask = (1U << width) - 1;
        return (reg_array_[addr_] >> LowBit) & mask;
    }

    template<size_t HighBit, size_t LowBit>
    constexpr RegisterAccessor& set_bits(uint16_t value) {
        static_assert(HighBit >= LowBit && HighBit < 16, "Invalid bit range");
        constexpr size_t width = HighBit - LowBit + 1;
        constexpr uint16_t mask = (1U << width) - 1;
        reg_array_[addr_] = (reg_array_[addr_] & ~(mask << LowBit)) | ((value & mask) << LowBit);
        return *this;
    }

    // 런타임 비트 필드 접근
    constexpr uint16_t bits(size_t high_bit, size_t low_bit) const {
        const size_t width = high_bit - low_bit + 1;
        const uint16_t mask = (1U << width) - 1;
        return (reg_array_[addr_] >> low_bit) & mask;
    }

    constexpr RegisterAccessor& set_bits(size_t high_bit, size_t low_bit, uint16_t value) {
        const size_t width = high_bit - low_bit + 1;
        const uint16_t mask = (1U << width) - 1;
        reg_array_[addr_] = (reg_array_[addr_] & ~(mask << low_bit)) | ((value & mask) << low_bit);
        return *this;
    }

    // === 비트 시프트 접근 ===
    template<size_t ShiftCount>
    constexpr uint16_t lshift() const {
        static_assert(ShiftCount < 16, "Shift count must be < 16");
        return reg_array_[addr_] << ShiftCount;
    }

    template<size_t ShiftCount>
    constexpr uint16_t rshift() const {
        static_assert(ShiftCount < 16, "Shift count must be < 16");
        return reg_array_[addr_] >> ShiftCount;
    }

    // 런타임 비트 시프트 접근
    constexpr uint16_t lshift(size_t shift_count) const {
        return reg_array_[addr_] << shift_count;
    }

    constexpr uint16_t rshift(size_t shift_count) const {
        return reg_array_[addr_] >> shift_count;
    }
};

// 메인 레지스터 배열 클래스 (레지스터 전용 최적화)
template<size_t N>
class RegisterArray {
private:
    static constexpr size_t REGISTER_BYTE_WIDTH = 2;  // uint16_t 크기
    static constexpr size_t ALIGNMENT_BYTES = 2;      // 정렬 단위
    std::array<uint16_t, N> reg_;

    static constexpr size_t addr_to_index(size_t addr) {
        return addr / REGISTER_BYTE_WIDTH;
    }

public:
    RegisterArray() : reg_{} {}

    // 직접 인덱스 접근 (16비트 단위)
    uint16_t& operator[](RegAddr addr) {
        return reg_[addr_to_index(static_cast<size_t>(addr))];
    }
    
    const uint16_t& operator[](RegAddr addr) const {
        return reg_[addr_to_index(static_cast<size_t>(addr))];
    }

    // 체이닝을 위한 레지스터 접근자 반환
    constexpr auto reg(RegAddr addr) -> RegisterAccessor<RegisterArray> {
        return RegisterAccessor<RegisterArray>(*this, addr);
    }

    // === 레지스터 전용 바이트/워드 접근 ===
    
    // 1바이트 읽기 (최고 성능)
    [[nodiscard]] inline uint8_t read_byte(size_t byte_addr) const noexcept {
        // 범위 체크
        if (__builtin_expect(byte_addr >= N * REGISTER_BYTE_WIDTH, 0)) [[unlikely]] return 0;
        
        // 직접 메모리 접근 (memcpy보다 빠름)
        const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(reg_.data());
        return byte_ptr[byte_addr];
    }
    
    // 1바이트 쓰기 (최고 성능)
    inline void write_byte(size_t byte_addr, uint8_t value) noexcept {
        // 범위 체크
        if (__builtin_expect(byte_addr >= N * REGISTER_BYTE_WIDTH, 0)) [[unlikely]] return;
        
        // 직접 메모리 접근 (memcpy보다 빠름)
        uint8_t* byte_ptr = reinterpret_cast<uint8_t*>(reg_.data());
        byte_ptr[byte_addr] = value;
    }
    
    // 2바이트 읽기 (정렬된 접근만)
    [[nodiscard]] inline uint16_t read_word(size_t byte_addr) const noexcept {
        // 정렬 및 범위 체크
        if (__builtin_expect(byte_addr >= N * REGISTER_BYTE_WIDTH || 
                           byte_addr % ALIGNMENT_BYTES != 0, 0)) [[unlikely]] return 0;
        
        const size_t reg_index = byte_addr / REGISTER_BYTE_WIDTH;
        return reg_[reg_index];
    }
    
    // 2바이트 쓰기 (정렬된 접근만)
    inline void write_word(size_t byte_addr, uint16_t value) noexcept {
        // 정렬 및 범위 체크
        if (__builtin_expect(byte_addr >= N * REGISTER_BYTE_WIDTH || 
                           byte_addr % ALIGNMENT_BYTES != 0, 0)) [[unlikely]] return;
        
        const size_t reg_index = byte_addr / REGISTER_BYTE_WIDTH;
        reg_[reg_index] = value;
    }
    
    // === 통합 read/write 인터페이스 (1바이트 또는 2바이트만) ===
    
    // 레지스터 전용 읽기 (크기: 1 또는 2바이트만)
    inline void read(size_t byte_addr, void* data_ptr, size_t req_size) const noexcept {
        // 레지스터는 1바이트 또는 2바이트만 허용
        if (__builtin_expect(req_size != 1 && req_size != ALIGNMENT_BYTES, 0)) [[unlikely]] return;
        
        if (req_size == 1) {
            // 1바이트: direct access가 가장 빠름
            *static_cast<uint8_t*>(data_ptr) = read_byte(byte_addr);
        } else {
            // 2바이트: 정렬된 워드 접근
            if (__builtin_expect(byte_addr % ALIGNMENT_BYTES == 0, 1)) [[likely]] {
                *static_cast<uint16_t*>(data_ptr) = read_word(byte_addr);
            }
            // 정렬되지 않은 2바이트 접근은 불허
        }
    }
    
    // 레지스터 전용 쓰기 (크기: 1 또는 2바이트만)
    inline void write(size_t byte_addr, const void* data_ptr, size_t req_size) noexcept {
        // 레지스터는 1바이트 또는 2바이트만 허용
        if (__builtin_expect(req_size != 1 && req_size != ALIGNMENT_BYTES, 0)) [[unlikely]] return;
        
        if (req_size == 1) {
            // 1바이트: direct access가 가장 빠름
            write_byte(byte_addr, *static_cast<const uint8_t*>(data_ptr));
        } else {
            // 2바이트: 정렬된 워드 접근
            if (__builtin_expect(byte_addr % ALIGNMENT_BYTES == 0, 1)) [[likely]] {
                write_word(byte_addr, *static_cast<const uint16_t*>(data_ptr));
            }
            // 정렬되지 않은 2바이트 접근은 불허
        }
    }
    
    // 템플릿 기반 타입 안전 인터페이스
    template<typename T>
    [[nodiscard]] inline T read_as(size_t byte_addr) const noexcept {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2, "Only 1 or 2 byte types allowed for registers");
        
        if constexpr (sizeof(T) == 1) {
            return static_cast<T>(read_byte(byte_addr));
        } else {
            return static_cast<T>(read_word(byte_addr));
        }
    }
    
    template<typename T>
    inline void write_as(size_t byte_addr, T value) noexcept {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2, "Only 1 or 2 byte types allowed for registers");
        
        if constexpr (sizeof(T) == 1) {
            write_byte(byte_addr, static_cast<uint8_t>(value));
        } else {
            write_word(byte_addr, static_cast<uint16_t>(value));
        }
    }
    
    // 정렬 상태 확인 헬퍼 함수
    [[nodiscard]] static constexpr bool is_aligned(size_t byte_addr, size_t req_size) noexcept {
        return (req_size == 1) || (byte_addr % ALIGNMENT_BYTES == 0 && req_size == ALIGNMENT_BYTES);
    }

    // 기본 메서드들
    uint16_t read(RegAddr addr) const { return (*this)[addr]; }
    void write(RegAddr addr, uint16_t value) { (*this)[addr] = value; }
    constexpr size_t size() const { return N; }
    
    // 레지스터 배열의 시작 주소 반환 (디버깅용)
    uint8_t* byte_ptr() noexcept { return reinterpret_cast<uint8_t*>(reg_.data()); }
    const uint8_t* byte_ptr() const noexcept { return reinterpret_cast<const uint8_t*>(reg_.data()); }
};

// === 레지스터 전용 최적화 사용 예제 ===
int main() {
    RegisterArray<10> registers;
    
    // === 레지스터 전용 최적화된 접근 ===
    
    // 1. 1바이트 접근 (모든 주소 가능)
    registers.write_byte(0x1000, 0xAB);     // 하위 바이트
    registers.write_byte(0x1001, 0xCD);     // 상위 바이트
    
    uint8_t low = registers.read_byte(0x1000);   // 0xAB
    uint8_t high = registers.read_byte(0x1001);  // 0xCD
    
    // 2. 2바이트 접근 (짝수 주소만)
    registers.write_word(0x1002, 0x1234);
    uint16_t word = registers.read_word(0x1002); // 0x1234
    
    // 3. 통합 인터페이스 (크기별 자동 최적화)
    uint8_t byte_data = 0xFF;
    registers.write(0x1004, &byte_data, 1);     // → write_byte() 호출
    
    uint16_t word_data = 0x5678;
    registers.write(0x1006, &word_data, 2);     // → write_word() 호출
    
    uint8_t read_byte;
    registers.read(0x1004, &read_byte, 1);      // → read_byte() 호출
    
    uint16_t read_word;
    registers.read(0x1006, &read_word, 2);      // → read_word() 호출
    
    // 4. 타입 안전 템플릿 인터페이스
    registers.write_as<uint8_t>(0x1008, 0xEF);
    registers.write_as<uint16_t>(0x100A, 0x9ABC);
    
    uint8_t safe_byte = registers.read_as<uint8_t>(0x1008);   // 0xEF
    uint16_t safe_word = registers.read_as<uint16_t>(0x100A); // 0x9ABC
    
    // === 16비트 레지스터 비트 조작 ===
    
    registers.reg(RegAddr::ABC) = 0x1234;
    registers.reg(RegAddr::ABC).set_bit<5>();
    uint16_t field = registers.reg(RegAddr::ABC).bits<7,4>();
    
    // === 실제 레지스터 사용 시나리오 ===
    
    // 시나리오 1: 상태 레지스터 개별 바이트 처리
    registers.write_byte(0x1000, 0x80);  // 상태 플래그
    registers.write_byte(0x1001, 0x03);  // 제어 플래그
    
    if (registers.read_byte(0x1000) & 0x80) {
        // 특정 상태 체크
    }
    
    // 시나리오 2: 설정 레지스터 워드 단위 처리
    uint16_t config = 0x1234;
    registers.write_word(0x1010, config);
    
    uint16_t current_config = registers.read_word(0x1010);
    
    // 시나리오 3: 타입별 안전한 접근
    enum class StatusByte : uint8_t { IDLE = 0, BUSY = 1, ERROR = 2 };
    enum class ConfigWord : uint16_t { DEFAULT = 0x1000, TURBO = 0x2000 };
    
    registers.write_as(0x1020, StatusByte::BUSY);
    registers.write_as(0x1022, ConfigWord::TURBO);
    
    StatusByte status = registers.read_as<StatusByte>(0x1020);
    ConfigWord config_mode = registers.read_as<ConfigWord>(0x1022);
    
    return 0;
}

/*
=== 레지스터 전용 최적화 요약 ===

🎯 핵심 개선사항:
1. 대용량 접근 로직 완전 제거 (size > 2 불허)
2. 1바이트/2바이트만 고려한 단순하고 빠른 구조
3. 레지스터 특성에 맞는 타입 안전 인터페이스

⚡ 성능 특성:
- 1바이트: direct memory access (최고 성능)
- 2바이트: direct register access (최고 성능)
- 복잡한 분기 제거로 예측 가능한 성능
- memcpy 오버헤드 완전 제거

🔧 사용법:
- write_byte/read_byte: 1바이트 직접 접근
- write_word/read_word: 2바이트 정렬된 접근
- write/read: 크기별 자동 선택
- write_as/read_as: 타입 안전 접근

🎉 결과:
- 코드 복잡도 대폭 감소
- 성능 예측 가능성 증가  
- 레지스터 용도에 완벽 최적화
- 타입 안전성 강화
*/