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
    // 상태 코드 정의
    static constexpr int SUCCESS = 0;
    static constexpr int ERROR_INVALID_ADDRESS = -1;
    static constexpr int ERROR_INVALID_SIZE = -2;
    static constexpr int ERROR_MISALIGNED = -3;

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

    // === 최적화된 원자적 접근 함수들 ===
    
    // 1바이트 읽기 (포인터 방식)
    [[nodiscard]] inline int read_byte(size_t byte_addr, uint8_t* value) const noexcept {
        // 널 포인터 체크
        if (value == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 범위 체크
        if (byte_addr >= N * REGISTER_BYTE_WIDTH) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
        
        // 직접 메모리 접근 (최고 성능)
        const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(reg_.data());
        *value = byte_ptr[byte_addr];
        return SUCCESS;
    }
    
    // 1바이트 쓰기 (포인터 방식)
    [[nodiscard]] inline int write_byte(size_t byte_addr, const uint8_t* value) noexcept {
        // 널 포인터 체크
        if (value == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 범위 체크
        if (byte_addr >= N * REGISTER_BYTE_WIDTH) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
        
        // 직접 메모리 접근 (최고 성능)
        uint8_t* byte_ptr = reinterpret_cast<uint8_t*>(reg_.data());
        byte_ptr[byte_addr] = *value;
        return SUCCESS;
    }
    
    // 2바이트 읽기 (포인터 방식, 정렬된 접근만)
    [[nodiscard]] inline int read_word(size_t byte_addr, uint16_t* value) const noexcept {
        // 널 포인터 체크
        if (value == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 정렬 체크
        if (byte_addr % ALIGNMENT_BYTES != 0) [[unlikely]] 
            return ERROR_MISALIGNED;
            
        // 범위 체크
        if (byte_addr >= N * REGISTER_BYTE_WIDTH) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
        
        const size_t reg_index = byte_addr / REGISTER_BYTE_WIDTH;
        *value = reg_[reg_index];
        return SUCCESS;
    }
    
    // 2바이트 쓰기 (포인터 방식, 정렬된 접근만)
    [[nodiscard]] inline int write_word(size_t byte_addr, const uint16_t* value) noexcept {
        // 널 포인터 체크
        if (value == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 정렬 체크
        if (byte_addr % ALIGNMENT_BYTES != 0) [[unlikely]] 
            return ERROR_MISALIGNED;
            
        // 범위 체크
        if (byte_addr >= N * REGISTER_BYTE_WIDTH) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
        
        const size_t reg_index = byte_addr / REGISTER_BYTE_WIDTH;
        reg_[reg_index] = *value;
        return SUCCESS;
    }
    
    // === 통합 read/write 인터페이스 (크기별 자동 선택) ===
    
    // 레지스터 전용 읽기 (크기: 1 또는 2바이트만)
    [[nodiscard]] inline int read(size_t byte_addr, void* data_ptr, size_t req_size) const noexcept {
        // 널 포인터 체크
        if (data_ptr == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 레지스터는 1바이트 또는 2바이트만 허용
        if (req_size != 1 && req_size != ALIGNMENT_BYTES) [[unlikely]] 
            return ERROR_INVALID_SIZE;
        
        if (req_size == 1) {
            // 1바이트: 최적화된 direct access
            return read_byte(byte_addr, static_cast<uint8_t*>(data_ptr));
        } else {
            // 2바이트: 정렬된 워드 접근
            return read_word(byte_addr, static_cast<uint16_t*>(data_ptr));
        }
    }
    
    // 레지스터 전용 쓰기 (크기: 1 또는 2바이트만)
    [[nodiscard]] inline int write(size_t byte_addr, const void* data_ptr, size_t req_size) noexcept {
        // 널 포인터 체크
        if (data_ptr == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 레지스터는 1바이트 또는 2바이트만 허용
        if (req_size != 1 && req_size != ALIGNMENT_BYTES) [[unlikely]] 
            return ERROR_INVALID_SIZE;
        
        if (req_size == 1) {
            // 1바이트: 최적화된 direct access
            return write_byte(byte_addr, static_cast<const uint8_t*>(data_ptr));
        } else {
            // 2바이트: 정렬된 워드 접근
            return write_word(byte_addr, static_cast<const uint16_t*>(data_ptr));
        }
    }
    
    // === 편의성을 위한 템플릿 인터페이스 ===
    
    // 타입 안전 읽기 (참조 방식으로 깔끔함)
    template<typename T>
    [[nodiscard]] inline int read_as(size_t byte_addr, T& value) const noexcept {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2, "Only 1 or 2 byte types allowed");
        
        if constexpr (sizeof(T) == 1) {
            return read_byte(byte_addr, reinterpret_cast<uint8_t*>(&value));
        } else {
            return read_word(byte_addr, reinterpret_cast<uint16_t*>(&value));
        }
    }
    
    // 타입 안전 쓰기 (값 방식으로 편리함)
    template<typename T>
    [[nodiscard]] inline int write_as(size_t byte_addr, const T& value) noexcept {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2, "Only 1 or 2 byte types allowed");
        
        if constexpr (sizeof(T) == 1) {
            return write_byte(byte_addr, reinterpret_cast<const uint8_t*>(&value));
        } else {
            return write_word(byte_addr, reinterpret_cast<const uint16_t*>(&value));
        }
    }
    
    // === 편의성 함수들 (기존 방식 호환) ===
    
    // 간편한 직접 값 읽기 (에러 시 기본값 반환)
    [[nodiscard]] inline uint8_t read_byte_safe(size_t byte_addr, uint8_t default_value = 0) const noexcept {
        uint8_t value;
        return (read_byte(byte_addr, &value) == SUCCESS) ? value : default_value;
    }
    
    [[nodiscard]] inline uint16_t read_word_safe(size_t byte_addr, uint16_t default_value = 0) const noexcept {
        uint16_t value;
        return (read_word(byte_addr, &value) == SUCCESS) ? value : default_value;
    }
    
    // 간편한 직접 값 쓰기 (bool 반환)
    [[nodiscard]] inline bool write_byte_simple(size_t byte_addr, uint8_t value) noexcept {
        return write_byte(byte_addr, &value) == SUCCESS;
    }
    
    [[nodiscard]] inline bool write_word_simple(size_t byte_addr, uint16_t value) noexcept {
        return write_word(byte_addr, &value) == SUCCESS;
    }
    
    // 정렬 상태 확인 헬퍼 함수
    [[nodiscard]] static constexpr bool is_aligned(size_t byte_addr, size_t req_size) noexcept {
        return (req_size == 1) || (byte_addr % ALIGNMENT_BYTES == 0 && req_size == ALIGNMENT_BYTES);
    }
    
    // 에러 코드를 문자열로 변환
    [[nodiscard]] static constexpr const char* error_string(int error_code) noexcept {
        switch (error_code) {
            case SUCCESS: return "Success";
            case ERROR_INVALID_ADDRESS: return "Invalid address";
            case ERROR_INVALID_SIZE: return "Invalid size";
            case ERROR_MISALIGNED: return "Misaligned access";
            default: return "Unknown error";
        }
    }

    // 기본 메서드들
    uint16_t read(RegAddr addr) const { return (*this)[addr]; }
    void write(RegAddr addr, uint16_t value) { (*this)[addr] = value; }
    constexpr size_t size() const { return N; }
    
    // 레지스터 배열의 시작 주소 반환 (디버깅용)
    uint8_t* byte_ptr() noexcept { return reinterpret_cast<uint8_t*>(reg_.data()); }
    const uint8_t* byte_ptr() const noexcept { return reinterpret_cast<const uint8_t*>(reg_.data()); }
};

// === 상태 코드 기반 레지스터 접근 사용 예제 ===
int main() {
    RegisterArray<10> registers;
    
    // === 기본 상태 코드 기반 접근 ===
    
    // 1. 1바이트 읽기/쓰기 (상태 코드 반환)
    uint8_t byte_value;
    int result = registers.read_byte(0x1000, &byte_value);
    if (result == RegisterArray<10>::SUCCESS) {
        std::cout << "Read successful: 0x" << std::hex << byte_value << std::endl;
    } else {
        std::cout << "Read failed: " << registers.error_string(result) << std::endl;
    }
    
    uint8_t write_data = 0xAB;
    result = registers.write_byte(0x1001, &write_data);
    if (result == RegisterArray<10>::SUCCESS) {
        std::cout << "Write successful" << std::endl;
    }
    
    // 2. 2바이트 읽기/쓰기 (정렬 체크 포함)
    uint16_t word_value;
    result = registers.read_word(0x1002, &word_value);  // 짝수 주소
    if (result == RegisterArray<10>::SUCCESS) {
        std::cout << "Word read: 0x" << std::hex << word_value << std::endl;
    }
    
    uint16_t write_word = 0x1234;
    result = registers.write_word(0x1003, &write_word);  // 홀수 주소 - 정렬 에러!
    if (result == RegisterArray<10>::ERROR_MISALIGNED) {
        std::cout << "Expected misalignment error occurred" << std::endl;
    }
    
    // === 통합 인터페이스 사용 ===
    
    // 3. 크기별 자동 선택
    uint8_t data1 = 0xFF;
    result = registers.write(0x1004, &data1, 1);  // 1바이트 → write_byte 호출
    
    uint16_t data2 = 0x5678;
    result = registers.write(0x1006, &data2, 2);  // 2바이트 → write_word 호출
    
    uint8_t read_data1;
    uint16_t read_data2;
    registers.read(0x1004, &read_data1, 1);      // 1바이트 읽기
    registers.read(0x1006, &read_data2, 2);      // 2바이트 읽기
    
    // === 템플릿 기반 타입 안전 접근 ===
    
    // 4. 참조 방식으로 깔끔한 읽기
    uint8_t byte_val;
    uint16_t word_val;
    
    result = registers.read_as(0x1008, byte_val);   // 타입 자동 추론
    if (result == RegisterArray<10>::SUCCESS) {
        std::cout << "Type-safe read: " << static_cast<int>(byte_val) << std::endl;
    }
    
    result = registers.read_as(0x100A, word_val);
    
    // 5. 값 방식으로 편리한 쓰기
    result = registers.write_as(0x100C, static_cast<uint8_t>(0xCD));
    result = registers.write_as(0x100E, static_cast<uint16_t>(0x9ABC));
    
    // === 편의성 함수들 ===
    
    // 6. 에러 시 기본값 반환 (간단한 경우)
    uint8_t safe_val = registers.read_byte_safe(0x1010, 0xFF);  // 에러 시 0xFF 반환
    uint16_t safe_word = registers.read_word_safe(0x1012, 0xDEAD);
    
    // 7. bool 반환으로 간단한 성공/실패 체크
    bool write_ok = registers.write_byte_simple(0x1014, 0x42);
    if (write_ok) {
        std::cout << "Simple write succeeded" << std::endl;
    }
    
    // === 에러 처리 패턴들 ===
    
    // 8. 상세한 에러 처리
    auto handle_register_error = [&](int error_code, const char* operation) {
        if (error_code != RegisterArray<10>::SUCCESS) {
            std::cerr << "Register " << operation << " failed: " 
                     << registers.error_string(error_code) << std::endl;
            return false;
        }
        return true;
    };
    
    uint8_t test_data = 0x55;
    if (handle_register_error(registers.write_byte(0x1016, &test_data), "write")) {
        uint8_t verify_data;
        if (handle_register_error(registers.read_byte(0x1016, &verify_data), "read")) {
            assert(verify_data == test_data);
            std::cout << "Write-verify cycle successful" << std::endl;
        }
    }
    
    // 9. 배치 처리
    struct RegisterWrite {
        size_t addr;
        uint8_t value;
    };
    
    RegisterWrite writes[] = {
        {0x1018, 0x11}, {0x1019, 0x22}, {0x101A, 0x33}
    };
    
    bool all_success = true;
    for (const auto& write : writes) {
        int result = registers.write_byte(write.addr, &write.value);
        if (result != RegisterArray<10>::SUCCESS) {
            std::cerr << "Batch write failed at address 0x" << std::hex << write.addr 
                     << ": " << registers.error_string(result) << std::endl;
            all_success = false;
        }
    }
    
    if (all_success) {
        std::cout << "Batch write completed successfully" << std::endl;
    }
    
    // === 16비트 레지스터 비트 조작은 그대로 ===
    
    registers.reg(RegAddr::ABC) = 0x1234;
    registers.reg(RegAddr::ABC).set_bit<5>();
    uint16_t field = registers.reg(RegAddr::ABC).bits<7,4>();
    
    // === 성능 및 에러 처리 혼합 사용 ===
    
    // 성능이 중요한 루프에서는 safe 버전 사용
    for (int i = 0; i < 100; ++i) {
        uint8_t data = static_cast<uint8_t>(i);
        if (!registers.write_byte_simple(0x1000 + (i % 20), data)) {
            std::cerr << "Write failed in performance loop at iteration " << i << std::endl;
            break;
        }
    }
    
    // 신뢰성이 중요한 곳에서는 상세한 에러 체크
    uint16_t critical_data = 0xCAFE;
    int critical_result = registers.write_word(0x1000, &critical_data);
    if (critical_result != RegisterArray<10>::SUCCESS) {
        std::cerr << "Critical write failed: " << registers.error_string(critical_result) << std::endl;
        // 에러 복구 로직...
        return -1;
    }
    
    return 0;
}

/*
=== 새로운 상태 코드 기반 설계 요약 ===

🎯 핵심 개선사항:
1. 명확한 에러 처리: 상태 코드로 실패 원인 구분
2. 포인터 기반 인터페이스: read/write 일관성 유지
3. 다양한 편의성 레벨: 성능 vs 편의성 선택 가능
4. 타입 안전성: 템플릿으로 컴파일 타임 체크

📊 성능 특성:
- 기본 함수들: 포인터 역참조 1회 + 직접 메모리 접근
- 템플릿 함수들: 컴파일 타임 최적화로 동일한 성능
- 편의성 함수들: 약간의 오버헤드로 사용성 향상
- 에러 체크: [[unlikely]]로 성능 영향 최소화

🔧 사용 패턴:
- 성능 중심: write_byte_simple(), read_byte_safe()
- 신뢰성 중심: write_byte() + 상태 코드 체크
- 타입 안전: read_as(), write_as() 템플릿
- 배치 처리: 배열 + 에러 누적 체크

🎉 결과:
- 명확한 에러 처리 + 기존 성능 유지
- 다양한 사용 시나리오 지원
- C 스타일 에러 처리와 현대 C++ 융합
- 디버깅 및 유지보수성 대폭 향상
*/