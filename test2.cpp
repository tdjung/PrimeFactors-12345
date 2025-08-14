// callgrind_generator_final.hpp
#ifndef CALLGRIND_GENERATOR_FINAL_HPP
#define CALLGRIND_GENERATOR_FINAL_HPP

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <stack>
#include <cstdint>
#include <unistd.h>
#include <string_view>

// Maximum number of events to track
constexpr size_t MAX_EVENTS = 10;

// Event types
enum EventType {
    EVENT_IR = 0,      // Instruction count
    EVENT_CYCLE = 1,   // Cycle count
    EVENT_BC = 2,      // Conditional branches
    EVENT_BCM = 3,     // Conditional branch mispredictions
    EVENT_BI = 4,      // Indirect branches
    EVENT_BIM = 5,     // Indirect branch mispredictions
};

// Branch types
enum class BranchType {
    NONE,
    BRANCH,           // Conditional branch
    DIRECT_JUMP,      // Direct unconditional jump
    INDIRECT_JUMP,    // Indirect jump
    CALL,             // Function call
    RETURN,           // Function return
    TAIL_CALL,        // Tail call
    FALL_THROUGH      // Fall-through to next function
};

// Function type for optimization
enum class FunctionType {
    NORMAL,
    SAVE_HELPER,
    RESTORE_HELPER
};

// Per-PC information from objdump
struct PCInfo {
    uint64_t pc;
    std::string func;
    std::string assembly;
    std::string file;
    uint32_t line;
    uint64_t event[MAX_EVENTS];
    FunctionType func_type;  // Cache function type
    
    PCInfo() : pc(0), line(0), func_type(FunctionType::NORMAL) {
        std::fill(std::begin(event), std::end(event), 0);
    }
};

// Target information for calls (includes inclusive costs)
struct CallTargetInfo {
    uint64_t count;
    uint64_t inclusive_events[MAX_EVENTS];
    bool is_fall_through;  // Track if this is a fall-through
    
    CallTargetInfo() : count(0), is_fall_through(false) {
        std::fill(std::begin(inclusive_events), std::end(inclusive_events), 0);
    }
};

// Branch information for conditional branches (always exactly 2 targets)
struct BranchInfo {
    uint64_t total_executed;
    uint64_t taken_target;
    uint64_t taken_count;
    uint64_t fallthrough_target;
    uint64_t fallthrough_count;
    
    BranchInfo() : total_executed(0), taken_target(0), taken_count(0), 
                   fallthrough_target(0), fallthrough_count(0) {}
};

// Call stack entry
struct CallStackEntry {
    uint64_t caller_pc;
    uint64_t callee_pc;
    std::string caller_func;
    std::string callee_func;
    uint64_t events_at_entry[MAX_EVENTS];
    bool is_tail_call;
    bool is_fall_through;  // Track if this was a fall-through entry
};

class CallgrindGenerator {
private:
    // Main data
    std::unordered_map<uint64_t, PCInfo> info;
    
    // Control flow tracking - unified structure
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, CallTargetInfo>> calls;  // from_pc -> (to_pc -> CallTargetInfo)
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> jumps;        // from_pc -> (to_pc -> count)
    std::unordered_map<uint64_t, BranchInfo> branches;                                 // from_pc -> BranchInfo
    
    // Runtime state
    std::stack<CallStackEntry> call_stack;
    uint64_t last_pc;
    int last_dest_reg;
    bool last_was_branch;
    uint32_t last_inst_size;
    uint64_t accumulated_events[MAX_EVENTS];
    std::string last_func_name;  // Track last function name for fall-through detection
    
    // Track the real caller when in compiler helper functions
    uint64_t real_caller_pc;
    std::string real_caller_func;
    
    // Configuration
    std::string output_filename;
    bool dump_instr;
    bool branch_sim;
    bool collect_jumps;
    
    // Event configuration
    std::vector<std::string> event_names;
    size_t num_events;
    
    // Constants for helper function detection
    static constexpr std::string_view SAVE_PREFIX = "__riscv_save";
    static constexpr std::string_view RESTORE_PREFIX = "__riscv_restore";
    
    // Determine function type from name (called once when loading)
    inline static FunctionType determineFunctionType(const std::string& func_name) {
        std::string_view sv(func_name);
        if (sv.substr(0, SAVE_PREFIX.size()) == SAVE_PREFIX) {
            return FunctionType::SAVE_HELPER;
        }
        if (sv.substr(0, RESTORE_PREFIX.size()) == RESTORE_PREFIX) {
            return FunctionType::RESTORE_HELPER;
        }
        return FunctionType::NORMAL;
    }
    
    // Fast inline checks using cached type
    inline bool isSaveHelper(FunctionType type) const {
        return type == FunctionType::SAVE_HELPER;
    }
    
    inline bool isRestoreHelper(FunctionType type) const {
        return type == FunctionType::RESTORE_HELPER;
    }
    
    inline bool isCompilerHelper(FunctionType type) const {
        return type != FunctionType::NORMAL;
    }
    
    // Detect instruction size
    inline uint32_t detectInstructionSize(const std::string& assembly) const {
        // RISC-V compressed instructions start with 'c.'
        return (assembly.find("c.") != std::string::npos || 
                assembly.find("\tc.") != std::string::npos) ? 2 : 4;
    }
    
    // Detect branch type
    BranchType detectBranchType(uint64_t from_pc, uint64_t to_pc, int dest_reg, bool is_sequential) {
        auto from_it = info.find(from_pc);
        auto to_it = info.find(to_pc);
        
        if (from_it == info.end() || to_it == info.end()) {
            return is_sequential ? BranchType::BRANCH : BranchType::DIRECT_JUMP;
        }
        
        const auto& from_info = from_it->second;
        const auto& to_info = to_it->second;
        const FunctionType from_type = from_info.func_type;
        const FunctionType to_type = to_info.func_type;
        
        // Special handling for compiler helpers
        if (isCompilerHelper(from_type)) {
            // Return from restore helper to normal function
            if (isRestoreHelper(from_type) && !isCompilerHelper(to_type)) {
                return BranchType::RETURN;
            }
            // Fall-through between restore helpers (e.g., restore_4 -> restore_0)
            if (isRestoreHelper(from_type) && isRestoreHelper(to_type) && is_sequential) {
                return BranchType::NONE;  // Internal flow within helpers
            }
        }
        
        // Check for function fall-through (sequential execution across function boundary)
        if (is_sequential && from_info.func != to_info.func && !isCompilerHelper(from_type)) {
            // This is a fall-through from one function to another
            return BranchType::FALL_THROUGH;
        }
        
        // Check for calls to compiler helpers
        if (!is_sequential && isCompilerHelper(to_type)) {
            if (isSaveHelper(to_type)) {
                return BranchType::CALL;
            }
            if (isRestoreHelper(to_type)) {
                return BranchType::TAIL_CALL;
            }
        }
        
        // Check for return
        if (!is_sequential && !call_stack.empty()) {
            const auto& stack_top = call_stack.top();
            auto caller_it = info.find(stack_top.caller_pc);
            if (caller_it != info.end() && to_info.func == caller_it->second.func) {
                return BranchType::RETURN;
            }
        }
        
        // Different function = call or tail call (but only for non-sequential)
        if (!is_sequential && from_info.func != to_info.func) {
            return (dest_reg == 0) ? BranchType::TAIL_CALL : BranchType::CALL;
        }
        
        // Same function
        if (is_sequential) {
            return BranchType::BRANCH;  // Not taken
        }
        
        // Backward jump = likely loop
        if (to_pc < from_pc) {
            return BranchType::BRANCH;
        }
        
        // Forward jump - small distance = likely branch
        return ((to_pc - from_pc) <= 32) ? BranchType::BRANCH : BranchType::DIRECT_JUMP;
    }
    
    // Handle branch/call
    void handleBranch(uint64_t from_pc, uint64_t to_pc, BranchType type, bool is_sequential) {
        // Skip internal flow within compiler helpers
        if (type == BranchType::NONE) {
            return;
        }
        
        auto from_it = info.find(from_pc);
        auto to_it = info.find(to_pc);
        
        std::string from_func = (from_it != info.end()) ? from_it->second.func : "unknown";
        std::string to_func = (to_it != info.end()) ? to_it->second.func : "unknown";
        FunctionType to_type = (to_it != info.end()) ? to_it->second.func_type : FunctionType::NORMAL;
        FunctionType from_type = (from_it != info.end()) ? from_it->second.func_type : FunctionType::NORMAL;
        
        switch (type) {
            case BranchType::CALL: {
                // Skip calls FROM compiler helpers (but not TO helpers)
                if (isCompilerHelper(from_type)) {
                    // If calling from save helper, remember the real caller
                    if (isSaveHelper(from_type) && !real_caller_func.empty()) {
                        // Use the real caller for this call
                        from_pc = real_caller_pc;
                        from_func = std::move(real_caller_func);
                        real_caller_pc = 0;
                        real_caller_func.clear();
                    } else {
                        // Calls from restore helpers are ignored
                        return;
                    }
                }
                
                // Remember real caller if calling a save helper
                if (isSaveHelper(to_type)) {
                    real_caller_pc = from_pc;
                    real_caller_func = from_func;
                    // But still record the call to the helper!
                }
                
                // Push to call stack
                CallStackEntry entry;
                entry.caller_pc = from_pc;
                entry.callee_pc = to_pc;
                entry.caller_func = std::move(from_func);
                entry.callee_func = std::move(to_func);
                entry.is_tail_call = false;
                entry.is_fall_through = false;
                std::copy(accumulated_events, accumulated_events + MAX_EVENTS, entry.events_at_entry);
                call_stack.push(std::move(entry));
                
                // Record call
                ++calls[from_pc][to_pc].count;
                break;
            }
            
            case BranchType::TAIL_CALL: {
                // Skip tail calls FROM restore helpers
                if (isCompilerHelper(from_type)) {
                    return;
                }
                
                // Record tail call (including to restore helpers)
                ++calls[from_pc][to_pc].count;
                
                if (!call_stack.empty()) {
                    CallStackEntry tail_entry;
                    tail_entry.caller_pc = from_pc;
                    tail_entry.callee_pc = to_pc;
                    tail_entry.caller_func = std::move(from_func);
                    tail_entry.callee_func = std::move(to_func);
                    tail_entry.is_tail_call = true;
                    tail_entry.is_fall_through = false;
                    std::copy(accumulated_events, accumulated_events + MAX_EVENTS, tail_entry.events_at_entry);
                    call_stack.push(std::move(tail_entry));
                }
                break;
            }
            
            case BranchType::FALL_THROUGH: {
                // Record fall-through as a special type of call
                auto& call_info = calls[from_pc][to_pc];
                ++call_info.count;
                call_info.is_fall_through = true;  // Mark as fall-through
                
                // Push to call stack (treat like a normal call for cost tracking)
                CallStackEntry entry;
                entry.caller_pc = from_pc;
                entry.callee_pc = to_pc;
                entry.caller_func = std::move(from_func);
                entry.callee_func = std::move(to_func);
                entry.is_tail_call = false;
                entry.is_fall_through = true;
                std::copy(accumulated_events, accumulated_events + MAX_EVENTS, entry.events_at_entry);
                call_stack.push(std::move(entry));
                break;
            }
            
            case BranchType::RETURN: {
                if (!call_stack.empty()) {
                    auto& entry = call_stack.top();
                    auto& call_info = calls[entry.caller_pc][entry.callee_pc];
                    
                    // Update inclusive costs directly
                    for (size_t i = 0; i < MAX_EVENTS; ++i) {
                        call_info.inclusive_events[i] += accumulated_events[i] - entry.events_at_entry[i];
                    }
                    
                    bool was_tail_call = entry.is_tail_call;
                    call_stack.pop();
                    
                    // Handle tail call chain
                    if (was_tail_call && !call_stack.empty()) {
                        auto& original_entry = call_stack.top();
                        auto& original_call = calls[original_entry.caller_pc][original_entry.callee_pc];
                        
                        // Update original call's inclusive costs directly
                        for (size_t i = 0; i < MAX_EVENTS; ++i) {
                            original_call.inclusive_events[i] += accumulated_events[i] - original_entry.events_at_entry[i];
                        }
                        
                        call_stack.pop();
                    }
                }
                break;
            }
            
            case BranchType::BRANCH: {
                if (collect_jumps) {
                    auto& branch = branches[from_pc];
                    ++branch.total_executed;
                    
                    if (is_sequential) {
                        branch.fallthrough_target = to_pc;
                        ++branch.fallthrough_count;
                    } else {
                        branch.taken_target = to_pc;
                        ++branch.taken_count;
                    }
                    
                    // Update branch statistics
                    ++info[from_pc].event[EVENT_BC];
                    
                    // Simple misprediction model - minority path
                    if (branch.taken_count > 0 && branch.fallthrough_count > 0) {
                        uint64_t minority = std::min(branch.taken_count, branch.fallthrough_count);
                        if (branch.total_executed > 1 && 
                            (minority == branch.taken_count || minority == branch.fallthrough_count)) {
                            ++info[from_pc].event[EVENT_BCM];
                        }
                    }
                }
                break;
            }
            
            case BranchType::DIRECT_JUMP:
            case BranchType::INDIRECT_JUMP: {
                // Skip jumps from compiler helpers
                if (isCompilerHelper(from_type)) {
                    return;
                }
                
                if (collect_jumps) {
                    ++jumps[from_pc][to_pc];
                    
                    if (type == BranchType::INDIRECT_JUMP) {
                        ++info[from_pc].event[EVENT_BI];
                        // Misprediction for indirect jumps with multiple targets
                        if (jumps[from_pc].size() > 1) {
                            ++info[from_pc].event[EVENT_BIM];
                        }
                    }
                }
                break;
            }
            
            default:
                break;
        }
    }
    
public:
    CallgrindGenerator(const std::string& filename = "callgrind.out") 
        : output_filename(filename),
          last_pc(0),
          last_dest_reg(-1),
          last_was_branch(false),
          last_inst_size(4),
          dump_instr(true),
          branch_sim(true),
          collect_jumps(true),
          num_events(2),
          real_caller_pc(0),
          last_func_name("") {
        
        event_names = {"Ir", "Cycle", "Bc", "Bcm", "Bi", "Bim"};
        std::fill(accumulated_events, accumulated_events + MAX_EVENTS, 0);
    }
    
    void setOptions(bool dump_instr_opt, bool branch_sim_opt, bool collect_jumps_opt) {
        dump_instr = dump_instr_opt;
        branch_sim = branch_sim_opt;
        collect_jumps = collect_jumps_opt;
    }
    
    void configureEvents(const std::vector<std::string>& names) {
        event_names = names;
        num_events = names.size();
    }
    
    // Load objdump data
    void loadPCInfo(uint64_t pc, const std::string& func, 
                    const std::string& assembly, const std::string& file, 
                    uint32_t line) {
        PCInfo& pc_info = info[pc];
        pc_info.pc = pc;
        pc_info.func = func;
        pc_info.assembly = assembly;
        pc_info.file = file;
        pc_info.line = line;
        pc_info.func_type = determineFunctionType(func);  // Cache function type
    }
    
    // Record instruction execution
    void recordExecution(uint64_t pc, EventType event_type, uint64_t count, 
                        int dest_reg = -1, bool is_branch_instruction = false) {
        auto it = info.find(pc);
        if (it == info.end()) {
            PCInfo& pc_info = info[pc];
            pc_info.pc = pc;
            pc_info.func = "unknown";
            pc_info.file = "unknown";
            pc_info.line = 0;
            pc_info.func_type = FunctionType::NORMAL;
            it = info.find(pc);  // Update iterator
        }
        
        // Get current function name
        std::string current_func = it->second.func;
        
        // Update events for ALL functions including helpers
        info[pc].event[event_type] += count;
        accumulated_events[event_type] += count;
        
        // Handle previous branch or function boundary crossing
        if (last_pc != 0) {
            bool function_changed = (!last_func_name.empty() && current_func != last_func_name);
            
            // Process if it was a branch OR function changed (fall-through detection)
            if (last_was_branch || function_changed) {
                bool is_sequential = (pc == last_pc + last_inst_size);
                BranchType branch_type = detectBranchType(last_pc, pc, last_dest_reg, is_sequential);
                handleBranch(last_pc, pc, branch_type, is_sequential);
            }
        }
        
        // Update state
        last_pc = pc;
        last_dest_reg = dest_reg;
        last_was_branch = is_branch_instruction;
        last_inst_size = it->second.assembly.empty() ? 4 : detectInstructionSize(it->second.assembly);
        last_func_name = current_func;
    }
    
    // Write output
    void writeOutput() {
        std::ofstream out(output_filename);
        if (!out.is_open()) {
            std::cerr << "Failed to open output file: " << output_filename << std::endl;
            return;
        }
        
        // Header
        out << "# callgrind format\n"
            << "version: 1\n"
            << "creator: core-simulator\n"
            << "pid: " << getpid() << "\n"
            << "cmd: simulated_program\n"
            << "part: 1\n\n"
            << "positions:";
        
        if (dump_instr) out << " instr";
        out << " line\n"
            << "events:";
        
        for (size_t i = 0; i < num_events && i < event_names.size(); ++i) {
            out << " " << event_names[i];
        }
        out << "\n\n";
        
        // Sort PCs - include ALL PCs
        std::vector<uint64_t> sorted_pcs;
        sorted_pcs.reserve(info.size());
        for (const auto& [pc, _] : info) {
            sorted_pcs.push_back(pc);
        }
        std::sort(sorted_pcs.begin(), sorted_pcs.end());
        
        // Write costs
        std::string current_func;
        std::string current_file;
        
        for (uint64_t pc : sorted_pcs) {
            const PCInfo& pc_info = info[pc];
            
            // Skip if no events
            bool has_events = false;
            for (size_t i = 0; i < MAX_EVENTS; ++i) {
                if (pc_info.event[i] > 0) {
                    has_events = true;
                    break;
                }
            }
            if (!has_events) continue;
            
            // Output function/file changes
            if (pc_info.func != current_func) {
                current_func = pc_info.func;
                out << "fn=" << current_func << "\n";
            }
            
            if (pc_info.file != current_file) {
                current_file = pc_info.file;
                out << "fl=" << current_file << "\n";
            }
            
            // Output position and events
            if (dump_instr) {
                out << "0x" << std::hex << pc << std::dec;
            }
            out << " " << pc_info.line;
            
            for (size_t i = 0; i < num_events; ++i) {
                out << " " << pc_info.event[i];
            }
            
            if (dump_instr && !pc_info.assembly.empty()) {
                out << " # " << pc_info.assembly;
            }
            out << "\n";
            
            // Output calls (skip calls FROM helper functions, but show calls TO helpers)
            if (!isCompilerHelper(pc_info.func_type)) {
                auto call_it = calls.find(pc);
                if (call_it != calls.end()) {
                    for (const auto& [target_pc, call_info] : call_it->second) {
                        auto callee_it = info.find(target_pc);
                        if (callee_it != info.end()) {
                            // Output all calls including to helpers
                            out << "cfn=" << callee_it->second.func;
                            if (call_info.is_fall_through) {
                                out << " [fall-through]";  // Mark fall-through calls
                            }
                            out << "\n"
                                << "cfl=" << callee_it->second.file << "\n";
                        } else {
                            out << "cfn=unknown\n";
                        }
                        
                        out << "calls=" << call_info.count << " ";
                        if (dump_instr) {
                            out << "0x" << std::hex << target_pc << std::dec;
                        }
                        out << " " << (callee_it != info.end() ? callee_it->second.line : 0) << "\n";
                        
                        // Inclusive costs
                        if (dump_instr) {
                            out << "0x" << std::hex << pc << std::dec;
                        }
                        out << " " << pc_info.line;
                        for (size_t i = 0; i < num_events; ++i) {
                            out << " " << call_info.inclusive_events[i];
                        }
                        out << "\n";
                    }
                }
            }
            
            // Output branches
            if (collect_jumps) {
                auto branch_it = branches.find(pc);
                if (branch_it != branches.end()) {
                    const auto& branch = branch_it->second;
                    
                    // Output taken target
                    if (branch.taken_count > 0) {
                        auto target_it = info.find(branch.taken_target);
                        out << "jcnd=" << branch.taken_count << "/" << branch.total_executed << " ";
                        if (dump_instr) {
                            out << "0x" << std::hex << branch.taken_target << std::dec;
                        }
                        out << " " << (target_it != info.end() ? target_it->second.line : 0) << "\n";
                    }
                    
                    // Output fallthrough target
                    if (branch.fallthrough_count > 0) {
                        auto target_it = info.find(branch.fallthrough_target);
                        out << "jcnd=" << branch.fallthrough_count << "/" << branch.total_executed << " ";
                        if (dump_instr) {
                            out << "0x" << std::hex << branch.fallthrough_target << std::dec;
                        }
                        out << " " << (target_it != info.end() ? target_it->second.line : 0) << "\n";
                    }
                }
                
                // Output unconditional jumps
                auto jump_it = jumps.find(pc);
                if (jump_it != jumps.end()) {
                    for (const auto& [target_pc, count] : jump_it->second) {
                        auto target_it = info.find(target_pc);
                        out << "jump=";
                        if (dump_instr) {
                            out << "0x" << std::hex << target_pc << std::dec;
                        }
                        out << "/" << (target_it != info.end() ? target_it->second.func : "unknown")
                            << " " << count << "\n";
                    }
                }
            }
        }
        
        // Summary
        out << "\n# Summary\n"
            << "totals:";
        uint64_t totals[MAX_EVENTS] = {0};
        for (const auto& [_, pc_info] : info) {
            for (size_t i = 0; i < num_events; ++i) {
                totals[i] += pc_info.event[i];
            }
        }
        for (size_t i = 0; i < num_events; ++i) {
            out << " " << totals[i];
        }
        out << "\n";
        
        out.close();
        std::cout << "Callgrind output written to: " << output_filename << std::endl;
    }
};

// Simulator interface
class SimulatorInterface {
private:
    CallgrindGenerator generator;
    
public:
    SimulatorInterface(const std::string& output_file = "callgrind.out.sim") 
        : generator(output_file) {
        generator.setOptions(true, true, true);
        generator.configureEvents({"Ir", "Cycle", "Bc", "Bcm", "Bi", "Bim"});
    }
    
    void loadObjdumpData(const std::vector<std::tuple<uint64_t, std::string, std::string, std::string, uint32_t>>& objdump_data) {
        for (const auto& [pc, func, assembly, file, line] : objdump_data) {
            generator.loadPCInfo(pc, func, assembly, file, line);
        }
    }
    
    void onInstruction(uint64_t pc, EventType event, uint64_t count, 
                      int dest_reg = -1, bool is_branch = false) {
        generator.recordExecution(pc, event, count, dest_reg, is_branch);
    }
    
    void finalize() {
        generator.writeOutput();
    }
};

#endif // CALLGRIND_GENERATOR_FINAL_HPP