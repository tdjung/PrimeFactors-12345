template<typename AddrType>
class RegisterArray {
private:
    static constexpr size_t REGISTER_BYTE_WIDTH = 2;
    static constexpr size_t BASE_ADDR = static_cast<size_t>(AddrType::REG_BASE);
    static constexpr size_t END_ADDR = static_cast<size_t>(AddrType::REG_END);
    static constexpr size_t REG_COUNT = (END_ADDR - BASE_ADDR) / REGISTER_BYTE_WIDTH;
    
    std::array<uint16_t, REG_COUNT> reg_;
    
    static constexpr size_t to_raw_addr(AddrType addr) noexcept {
        return static_cast<size_t>(addr);
    }
    
    static constexpr size_t addr_to_index(size_t addr) {
        return (addr - BASE_ADDR) / REGISTER_BYTE_WIDTH;
    }
    
    static constexpr bool is_valid_addr(size_t addr) {
        return addr >= BASE_ADDR && 
               addr < END_ADDR && 
               (addr - BASE_ADDR) % REGISTER_BYTE_WIDTH == 0;
    }

public:
    using AddressType = AddrType;
    using AccessorType = RegisterAccessor<RegisterArray, AddrType>;

    RegisterArray() : reg_{} {}

    // === 1. 기존 enum 접근 ===
    uint16_t& operator[](AddrType addr) {
        size_t raw_addr = to_raw_addr(addr);
        if (!is_valid_addr(raw_addr)) {
            static uint16_t dummy = 0;
            return dummy;
        }
        return reg_[addr_to_index(raw_addr)];
    }

    // === 2. 원시 주소 접근 (새로 추가!) ===
    uint16_t& at_addr(size_t raw_addr) {
        if (!is_valid_addr(raw_addr)) {
            static uint16_t dummy = 0;
            return dummy;
        }
        return reg_[addr_to_index(raw_addr)];
    }
    
    const uint16_t& at_addr(size_t raw_addr) const {
        if (!is_valid_addr(raw_addr)) {
            static const uint16_t dummy = 0;
            return dummy;
        }
        return reg_[addr_to_index(raw_addr)];
    }

    // === 3. 계산된 주소 접근 ===
    uint16_t& at_offset(AddrType base_addr, ptrdiff_t offset) {
        size_t raw_addr = to_raw_addr(base_addr) + offset;
        return at_addr(raw_addr);
    }
    
    const uint16_t& at_offset(AddrType base_addr, ptrdiff_t offset) const {
        size_t raw_addr = to_raw_addr(base_addr) + offset;
        return at_addr(raw_addr);
    }

    // === 4. 비트 조작용 접근자들 ===
    constexpr AccessorType reg(AddrType addr) {
        return AccessorType(*this, addr);
    }
    
    // 원시 주소용 접근자
    constexpr AccessorType reg_at(size_t raw_addr) {
        return AccessorType(*this, static_cast<AddrType>(raw_addr));
    }
    
    // 오프셋용 접근자 (비트 조작 가능!)
    constexpr AccessorType reg_offset(AddrType base_addr, ptrdiff_t offset) {
        size_t raw_addr = to_raw_addr(base_addr) + offset;
        return AccessorType(*this, static_cast<AddrType>(raw_addr));
    }
    
    // 인덱스용 접근자
    constexpr AccessorType reg_index(size_t index) {
        size_t raw_addr = BASE_ADDR + (index * REGISTER_BYTE_WIDTH);
        return AccessorType(*this, static_cast<AddrType>(raw_addr));
    }

    // === 5. 배열 스타일 접근 (인덱스 기반) ===
    uint16_t& at_index(size_t index) {
        if (index >= REG_COUNT) {
            static uint16_t dummy = 0;
            return dummy;
        }
        return reg_[index];
    }
    
    const uint16_t& at_index(size_t index) const {
        if (index >= REG_COUNT) {
            static const uint16_t dummy = 0;
            return dummy;
        }
        return reg_[index];
    }

    // 유틸리티 함수들
    static constexpr size_t addr_to_index_public(size_t addr) {
        return addr_to_index(addr);
    }
    
    static constexpr size_t index_to_addr(size_t index) {
        return BASE_ADDR + (index * REGISTER_BYTE_WIDTH);
    }

    static constexpr size_t size() noexcept { return REG_COUNT; }
    static constexpr size_t base_addr() noexcept { return BASE_ADDR; }
};

// === 사용 예제 ===
namespace TestModule {
    enum class RegAddr : size_t {
        REG_BASE = 0x1000,
        CONFIG = 0x1000,     // 인덱스 0
        DATA_0 = 0x1002,     // 인덱스 1  
        DATA_1 = 0x1004,     // 인덱스 2
        DATA_2 = 0x1006,     // 인덱스 3
        DATA_3 = 0x1008,     // 인덱스 4
        STATUS = 0x100A,     // 인덱스 5
        REG_END = 0x100C
    };
    
    using Registers = RegisterArray<RegAddr>;
}

int main() {
    TestModule::Registers regs;
    
    std::cout << "=== 다양한 동적 접근 방법들 ===" << std::endl;
    
    // === 방법 1: 원시 주소 접근 ===
    std::cout << "\n1. 원시 주소 접근:" << std::endl;
    for (int i = 0; i < 4; ++i) {
        size_t addr = 0x1002 + 2*i;  // DATA_0, DATA_1, DATA_2, DATA_3
        regs.at_addr(addr) = 0x1000 + i;
        std::cout << "  addr 0x" << std::hex << addr 
                  << " = 0x" << regs.at_addr(addr) << std::endl;
    }
    
    // === 방법 2: 오프셋 접근 ===
    std::cout << "\n2. 오프셋 접근:" << std::endl;
    for (int i = 0; i < 4; ++i) {
        ptrdiff_t offset = 2 + 2*i;  // CONFIG부터 +2, +4, +6, +8
        regs.at_offset(TestModule::RegAddr::CONFIG, offset) = 0x2000 + i;
        std::cout << "  CONFIG+" << std::dec << offset 
                  << " = 0x" << std::hex << regs.at_offset(TestModule::RegAddr::CONFIG, offset) << std::endl;
    }
    
    // === 방법 3: 인덱스 접근 ===
    std::cout << "\n3. 인덱스 접근:" << std::endl;
    for (size_t i = 1; i <= 4; ++i) {  // DATA_0~DATA_3 (인덱스 1~4)
        regs.at_index(i) = 0x3000 + i;
        std::cout << "  index[" << std::dec << i 
                  << "] = 0x" << std::hex << regs.at_index(i) << std::endl;
    }
    
    // === 방법 4: 비트 조작도 가능 ===
    std::cout << "\n4. 동적 비트 조작:" << std::endl;
    for (int i = 0; i < 4; ++i) {
        size_t addr = 0x1002 + 2*i;
        regs.reg_at(addr).set_bit(i, true);  // 각각 다른 비트 설정
        bool bit_val = regs.reg_at(addr).bit(i);
        std::cout << "  addr 0x" << std::hex << addr 
                  << " bit[" << std::dec << i << "] = " << bit_val << std::endl;
    }
    
    // === 방법 5: 기존 enum과 혼용 ===
    std::cout << "\n5. enum과 동적 접근 혼용:" << std::endl;
    
    // enum으로 베이스 주소 얻기
    size_t base = static_cast<size_t>(TestModule::RegAddr::DATA_0);
    
    // 동적으로 접근
    for (int i = 0; i < 3; ++i) {
        regs.at_addr(base + 2*i) = 0x4000 + i;
        std::cout << "  DATA_" << i << " = 0x" << std::hex 
                  << regs.at_addr(base + 2*i) << std::endl;
    }
    
    // === 실제 사용 패턴 예제 ===
    std::cout << "\n=== 실제 사용 패턴 ===" << std::endl;
    
    // 기존 방식을 대체하는 새로운 방식들
    constexpr size_t DATA_BASE = static_cast<size_t>(TestModule::RegAddr::DATA_0);
    
    std::cout << "기존: reg[0x1002 + 2*i]" << std::endl;
    std::cout << "신규1: regs.at_addr(0x1002 + 2*i)" << std::endl;
    std::cout << "신규2: regs.at_addr(DATA_BASE + 2*i)" << std::endl;
    std::cout << "신규3: regs.at_offset(RegAddr::DATA_0, 2*i)" << std::endl;
    std::cout << "신규4: regs.at_index(1 + i)  // DATA_0은 인덱스 1" << std::endl;
    
    return 0;
}