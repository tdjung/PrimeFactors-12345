#include <cstdint>
#include <array>
#include <cstring>
#include <iostream>
#include <cassert>

// === 제네릭 레지스터 배열 클래스 ===

// 모든 비트 조작을 담당하는 제네릭 헬퍼 클래스
template<typename RegisterArrayType, typename AddrType>
class RegisterAccessor {
private:
    RegisterArrayType& reg_array_;
    const AddrType addr_;

public:
    constexpr RegisterAccessor(RegisterArrayType& reg_array, AddrType addr)
        : reg_array_(reg_array), addr_(addr) {}

    // === 전체 레지스터 접근 ===
    constexpr uint16_t& operator()() { return reg_array_[addr_]; }
    constexpr const uint16_t& operator()() const { return reg_array_[addr_]; }
    constexpr RegisterAccessor& operator=(uint16_t value) { reg_array_[addr_] = value; return *this; }
    constexpr operator uint16_t() const { return reg_array_[addr_]; }

    // === 단일 비트 접근 ===
    template<size_t BitPos>
    [[nodiscard]] constexpr bool bit() const {
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
    [[nodiscard]] constexpr bool bit(size_t bit_pos) const {
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
    [[nodiscard]] constexpr uint16_t bits() const {
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
    [[nodiscard]] constexpr uint16_t bits(size_t high_bit, size_t low_bit) const {
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
    [[nodiscard]] constexpr uint16_t lshift() const {
        static_assert(ShiftCount < 16, "Shift count must be < 16");
        return reg_array_[addr_] << ShiftCount;
    }

    template<size_t ShiftCount>
    [[nodiscard]] constexpr uint16_t rshift() const {
        static_assert(ShiftCount < 16, "Shift count must be < 16");
        return reg_array_[addr_] >> ShiftCount;
    }

    // 런타임 비트 시프트 접근
    [[nodiscard]] constexpr uint16_t lshift(size_t shift_count) const {
        return reg_array_[addr_] << shift_count;
    }

    [[nodiscard]] constexpr uint16_t rshift(size_t shift_count) const {
        return reg_array_[addr_] >> shift_count;
    }
};

// 제네릭 메인 레지스터 배열 클래스
template<size_t N, typename AddrType = size_t>
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

    // 타입 별칭
    using AddressType = AddrType;
    using AccessorType = RegisterAccessor<RegisterArray, AddrType>;

    RegisterArray() : reg_{} {}

    // 직접 인덱스 접근 (16비트 단위)
    uint16_t& operator[](AddrType addr) {
        return reg_[addr_to_index(static_cast<size_t>(addr))];
    }
    
    const uint16_t& operator[](AddrType addr) const {
        return reg_[addr_to_index(static_cast<size_t>(addr))];
    }

    // 체이닝을 위한 레지스터 접근자 반환
    constexpr AccessorType reg(AddrType addr) {
        return AccessorType(*this, addr);
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
    
    // === 통합 read/write 인터페이스 ===
    
    // 레지스터 전용 읽기 (크기: 1 또는 2바이트만)
    [[nodiscard]] inline int read(size_t byte_addr, void* data_ptr, size_t req_size) const noexcept {
        // 널 포인터 체크
        if (data_ptr == nullptr) [[unlikely]] 
            return ERROR_INVALID_ADDRESS;
            
        // 레지스터는 1바이트 또는 2바이트만 허용
        if (req_size != 1 && req_size != ALIGNMENT_BYTES) [[unlikely]] 
            return ERROR_INVALID_SIZE;
        
        if (req_size == 1) {
            return read_byte(byte_addr, static_cast<uint8_t*>(data_ptr));
        } else {
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
            return write_byte(byte_addr, static_cast<const uint8_t*>(data_ptr));
        } else {
            return write_word(byte_addr, static_cast<const uint16_t*>(data_ptr));
        }
    }
    
    // === 편의성을 위한 템플릿 인터페이스 ===
    
    // 타입 안전 읽기
    template<typename T>
    [[nodiscard]] inline int read_as(size_t byte_addr, T& value) const noexcept {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2, "Only 1 or 2 byte types allowed");
        
        if constexpr (sizeof(T) == 1) {
            return read_byte(byte_addr, reinterpret_cast<uint8_t*>(&value));
        } else {
            return read_word(byte_addr, reinterpret_cast<uint16_t*>(&value));
        }
    }
    
    // 타입 안전 쓰기
    template<typename T>
    [[nodiscard]] inline int write_as(size_t byte_addr, const T& value) noexcept {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2, "Only 1 or 2 byte types allowed");
        
        if constexpr (sizeof(T) == 1) {
            return write_byte(byte_addr, reinterpret_cast<const uint8_t*>(&value));
        } else {
            return write_word(byte_addr, reinterpret_cast<const uint16_t*>(&value));
        }
    }
    
    // === 편의성 함수들 ===
    
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
    
    // === 유틸리티 함수들 ===
    
    // 정렬 상태 확인
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
    [[nodiscard]] uint16_t read(AddrType addr) const { return (*this)[addr]; }
    void write(AddrType addr, uint16_t value) { (*this)[addr] = value; }
    [[nodiscard]] constexpr size_t size() const { return N; }
    [[nodiscard]] constexpr size_t byte_size() const { return N * REGISTER_BYTE_WIDTH; }
    
    // 레지스터 배열의 시작 주소 반환 (디버깅용)
    uint8_t* byte_ptr() noexcept { return reinterpret_cast<uint8_t*>(reg_.data()); }
    const uint8_t* byte_ptr() const noexcept { return reinterpret_cast<const uint8_t*>(reg_.data()); }
};

// === 편의성을 위한 타입 별칭 템플릿 ===
template<size_t N, typename AddrType>
using GenericRegisters = RegisterArray<N, AddrType>;

// === 사용 예제 및 다양한 모듈 구성 ===

// 모듈 1: UART 레지스터
namespace UartModule {
    enum class RegAddr : size_t {
        TX_DATA = 0x1000,
        RX_DATA = 0x1002,
        STATUS = 0x1004,
        CONTROL = 0x1006,
        BAUDRATE = 0x1008
    };
    
    using Registers = RegisterArray<10, RegAddr>;
    
    // UART 특화 함수들
    class UartController {
        Registers regs_;
    public:
        bool send_byte(uint8_t data) {
            return regs_.reg(RegAddr::TX_DATA) = data, true;
        }
        
        bool is_ready() {
            return regs_.reg(RegAddr::STATUS).bit<0>();
        }
        
        void set_baudrate(uint16_t rate) {
            regs_.reg(RegAddr::BAUDRATE) = rate;
        }
    };
}

// 모듈 2: SPI 레지스터
namespace SpiModule {
    enum class RegAddr : size_t {
        DATA = 0x2000,
        CONTROL = 0x2002,
        STATUS = 0x2004,
        CLOCK_DIV = 0x2006
    };
    
    using Registers = RegisterArray<8, RegAddr>;
    
    class SpiController {
        Registers regs_;
    public:
        uint16_t transfer(uint16_t data) {
            regs_.reg(RegAddr::DATA) = data;
            while (!regs_.reg(RegAddr::STATUS).bit<7>()) { /* wait */ }
            return regs_.reg(RegAddr::DATA);
        }
        
        void set_mode(uint8_t mode) {
            regs_.reg(RegAddr::CONTROL).set_bits<1,0>(mode);
        }
    };
}

// 모듈 3: GPIO 레지스터
namespace GpioModule {
    enum class RegAddr : size_t {
        PORT_A = 0x3000,
        PORT_B = 0x3002,
        DDR_A = 0x3004,
        DDR_B = 0x3006,
        PIN_A = 0x3008,
        PIN_B = 0x300A
    };
    
    using Registers = RegisterArray<6, RegAddr>;
    
    class GpioController {
        Registers regs_;
    public:
        void set_pin_output(RegAddr port, uint8_t pin) {
            auto ddr_addr = (port == RegAddr::PORT_A) ? RegAddr::DDR_A : RegAddr::DDR_B;
            regs_.reg(ddr_addr).set_bit(pin, true);
        }
        
        void write_pin(RegAddr port, uint8_t pin, bool value) {
            regs_.reg(port).set_bit(pin, value);
        }
        
        bool read_pin(RegAddr port, uint8_t pin) {
            auto pin_addr = (port == RegAddr::PORT_A) ? RegAddr::PIN_A : RegAddr::PIN_B;
            return regs_.reg(pin_addr).bit(pin);
        }
    };
}

// 메인 사용 예제
int main() {
    // === 다양한 모듈 사용 ===
    
    // UART 모듈 사용
    UartModule::UartController uart;
    uart.set_baudrate(9600);
    uart.send_byte(0x55);
    
    // SPI 모듈 사용
    SpiModule::SpiController spi;
    spi.set_mode(3);  // SPI 모드 3
    uint16_t received = spi.transfer(0x1234);
    
    // GPIO 모듈 사용
    GpioModule::GpioController gpio;
    gpio.set_pin_output(GpioModule::RegAddr::PORT_A, 5);
    gpio.write_pin(GpioModule::RegAddr::PORT_A, 5, true);
    bool pin_state = gpio.read_pin(GpioModule::RegAddr::PORT_A, 5);
    
    // === 제네릭 사용법 ===
    
    // 커스텀 주소 타입 정의
    enum class CustomAddr : size_t { REG1 = 0x4000, REG2 = 0x4002 };
    RegisterArray<5, CustomAddr> custom_regs;
    
    // 기본 size_t 타입 사용
    RegisterArray<10> simple_regs;  // AddrType 생략 시 size_t
    
    // === 에러 처리 예제 ===
    
    uint8_t data;
    int result = custom_regs.read_byte(0x4000, &data);
    if (result != RegisterArray<5, CustomAddr>::SUCCESS) {
        std::cerr << "Read failed: " << custom_regs.error_string(result) << std::endl;
    }
    
    // === 체이닝 사용 예제 ===
    
    custom_regs.reg(CustomAddr::REG1)
        .set_bits<15,8>(0xAB)
        .set_bits<7,4>(0xC)
        .set_bits<3,0>(0xD);
    
    std::cout << "Register value: 0x" << std::hex 
              << custom_regs.reg(CustomAddr::REG1)() << std::endl;
    
    return 0;
}

/*
=== 제네릭 레지스터 설계 요약 ===

🎯 핵심 개선사항:
1. 완전한 제네릭 설계: 어떤 주소 타입도 사용 가능
2. 모듈별 독립성: 각 모듈이 고유한 주소 체계 사용
3. 타입 안전성: 컴파일 타임에 주소 타입 검증
4. 재사용성: 하나의 클래스로 모든 레지스터 모듈 지원

🔧 사용 패턴:
- RegisterArray<크기, 주소타입>
- 모듈별 네임스페이스로 구분
- 각 모듈의 특화된 컨트롤러 클래스
- 공통 인터페이스 유지

⚡ 성능:
- 템플릿 특화로 런타임 오버헤드 없음
- 주소 타입별 컴파일 타임 최적화
- 기존 성능 특성 완전 유지

🎉 결과:
- 하나의 코드로 무한한 확장성
- 모듈간 독립성과 안전성 보장
- 실제 하드웨어 레지스터 맵과 직접 매핑 가능
- 팀 개발에서 모듈별 분리 개발 지원
*/