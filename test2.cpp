// callgrind_generator_simple.hpp
#ifndef CALLGRIND_GENERATOR_SIMPLE_HPP
#define CALLGRIND_GENERATOR_SIMPLE_HPP

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
    TAIL_CALL         // Tail call
};

// Per-PC information from objdump
struct PCInfo {
    uint64_t pc;
    std::string func;
    std::string assembly;
    std::string file;
    uint32_t line;
    uint64_t event[MAX_EVENTS];
    
    PCInfo() : pc(0), line(0) {
        std::fill(std::begin(event), std::end(event), 0);
    }
};

// Generic target information for calls/jumps
struct TargetInfo {
    uint64_t count;
    uint64_t inclusive_events[MAX_EVENTS];  // Only used for calls
    
    TargetInfo() : count(0) {
        std::fill(std::begin(inclusive_events), std::end(inclusive_events), 0);
    }
};

// Branch information for conditional branches (always exactly 2 targets)
struct BranchInfo {
    uint64_t total_executed;     // Total times this branch executed
    uint64_t taken_target;        // Target when taken
    uint64_t taken_count;         // Times taken
    uint64_t fallthrough_target;  // Target when not taken (next instruction)
    uint64_t fallthrough_count;   // Times not taken
    
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
};

class CallgrindGenerator {
private:
    // Main data
    std::unordered_map<uint64_t, PCInfo> info;
    
    // Control flow tracking - unified structure
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, TargetInfo>> calls;  // from_pc -> (to_pc -> TargetInfo)
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> jumps;    // from_pc -> (to_pc -> count)
    std::unordered_map<uint64_t, BranchInfo> branches;                             // from_pc -> BranchInfo
    
    // Runtime state
    std::stack<CallStackEntry> call_stack;
    std::vector<std::pair<uint64_t, uint64_t>> tail_call_chain;
    uint64_t last_pc;
    int last_dest_reg;
    bool last_was_branch;
    uint32_t last_inst_size;
    uint64_t accumulated_events[MAX_EVENTS];
    
    // Configuration
    std::string output_filename;
    bool dump_instr;
    bool branch_sim;
    bool collect_jumps;
    
    // Event configuration
    std::vector<std::string> event_names;
    size_t num_events;
    
    // Detect instruction size
    uint32_t detectInstructionSize(const std::string& assembly) {
        if (assembly.find("c.") != std::string::npos || 
            assembly.find("\tc.") != std::string::npos) {
            return 2;  // RISC-V compressed
        }
        return 4;
    }
    
    // Detect branch type
    BranchType detectBranchType(uint64_t from_pc, uint64_t to_pc, int dest_reg, bool is_sequential) {
        auto from_it = info.find(from_pc);
        auto to_it = info.find(to_pc);
        
        if (from_it == info.end() || to_it == info.end()) {
            if (is_sequential) {
                return BranchType::BRANCH;  // Assume conditional branch not taken
            }
            return BranchType::DIRECT_JUMP;
        }
        
        const std::string& from_func = from_it->second.func;
        const std::string& to_func = to_it->second.func;
        
        // Check for return
        if (!is_sequential && !call_stack.empty()) {
            const auto& stack_top = call_stack.top();
            auto caller_it = info.find(stack_top.caller_pc);
            if (caller_it != info.end() && to_func == caller_it->second.func) {
                return BranchType::RETURN;
            }
        }
        
        // Different function = call or tail call
        if (!is_sequential && from_func != to_func) {
            if (dest_reg == 0) {
                return BranchType::TAIL_CALL;
            }
            return BranchType::CALL;
        }
        
        // Same function
        if (is_sequential) {
            return BranchType::BRANCH;  // Not taken
        }
        
        // Backward jump = likely loop (conditional)
        if (to_pc < from_pc) {
            return BranchType::BRANCH;
        }
        
        // Forward jump
        uint64_t jump_distance = to_pc - from_pc;
        if (jump_distance <= 32) {
            return BranchType::BRANCH;
        }
        
        return BranchType::DIRECT_JUMP;
    }
    
    // Handle branch/call
    void handleBranch(uint64_t from_pc, uint64_t to_pc, BranchType type, bool is_sequential) {
        switch (type) {
            case BranchType::CALL: {
                // Push to call stack
                CallStackEntry entry;
                entry.caller_pc = from_pc;
                entry.callee_pc = to_pc;
                entry.caller_func = info[from_pc].func;
                entry.callee_func = info[to_pc].func;
                entry.is_tail_call = false;
                std::copy(std::begin(accumulated_events), std::end(accumulated_events), 
                         std::begin(entry.events_at_entry));
                call_stack.push(entry);
                
                // Record call
                auto& call_site = calls[from_pc];
                auto target_it = std::find_if(call_site.targets.begin(), call_site.targets.end(),
                    [to_pc](const CallTarget& t) { return t.target_pc == to_pc; });
                
                if (target_it != call_site.targets.end()) {
                    target_it->count++;
                } else {
                    CallTarget new_target;
                    new_target.target_pc = to_pc;
                    new_target.count = 1;
                    call_site.targets.push_back(new_target);
                }
                break;
            }
            
            case BranchType::TAIL_CALL: {
                tail_call_chain.push_back({from_pc, to_pc});
                
                if (!call_stack.empty()) {
                    auto& original_entry = call_stack.top();
                    
                    CallStackEntry tail_entry;
                    tail_entry.caller_pc = original_entry.caller_pc;
                    tail_entry.callee_pc = to_pc;
                    tail_entry.caller_func = original_entry.caller_func;
                    tail_entry.callee_func = info[to_pc].func;
                    tail_entry.is_tail_call = true;
                    std::copy(std::begin(original_entry.events_at_entry),
                             std::end(original_entry.events_at_entry),
                             std::begin(tail_entry.events_at_entry));
                    
                    call_stack.pop();
                    call_stack.push(tail_entry);
                    
                    auto& call_site = calls[from_pc];
                    auto target_it = std::find_if(call_site.targets.begin(), call_site.targets.end(),
                        [to_pc](const CallTarget& t) { return t.target_pc == to_pc; });
                    
                    if (target_it != call_site.targets.end()) {
                        target_it->count++;
                    } else {
                        CallTarget new_target;
                        new_target.target_pc = to_pc;
                        new_target.count = 1;
                        call_site.targets.push_back(new_target);
                    }
                }
                break;
            }
            
            case BranchType::RETURN: {
                if (!call_stack.empty()) {
                    auto& entry = call_stack.top();
                    
                    // Calculate inclusive cost
                    uint64_t inclusive_cost[MAX_EVENTS];
                    for (size_t i = 0; i < MAX_EVENTS; ++i) {
                        inclusive_cost[i] = accumulated_events[i] - entry.events_at_entry[i];
                    }
                    
                    // Update call record
                    auto call_it = calls.find(entry.caller_pc);
                    if (call_it != calls.end()) {
                        auto target_it = std::find_if(call_it->second.targets.begin(),
                                                      call_it->second.targets.end(),
                            [&entry](const CallTarget& t) { return t.target_pc == entry.callee_pc; });
                        
                        if (target_it != call_it->second.targets.end()) {
                            for (size_t i = 0; i < MAX_EVENTS; ++i) {
                                target_it->inclusive_events[i] += inclusive_cost[i];
                            }
                        }
                    }
                    
                    // Handle tail call chain
                    if (entry.is_tail_call && !tail_call_chain.empty()) {
                        for (const auto& [tail_from, tail_to] : tail_call_chain) {
                            auto tail_call_it = calls.find(tail_from);
                            if (tail_call_it != calls.end()) {
                                auto target_it = std::find_if(tail_call_it->second.targets.begin(),
                                                             tail_call_it->second.targets.end(),
                                    [tail_to](const CallTarget& t) { return t.target_pc == tail_to; });
                                
                                if (target_it != tail_call_it->second.targets.end()) {
                                    for (size_t i = 0; i < MAX_EVENTS; ++i) {
                                        target_it->inclusive_events[i] += 
                                            inclusive_cost[i] / (tail_call_chain.size() + 1);
                                    }
                                }
                            }
                        }
                        tail_call_chain.clear();
                    }
                    
                    call_stack.pop();
                }
                break;
            }
            
            case BranchType::BRANCH: {
                if (collect_jumps) {
                    auto& branch = branches[from_pc];
                    branch.total_executed++;
                    
                    if (is_sequential) {
                        // Not taken - fallthrough
                        branch.fallthrough_target = to_pc;
                        branch.fallthrough_count++;
                    } else {
                        // Taken
                        branch.taken_target = to_pc;
                        branch.taken_count++;
                    }
                    
                    // Update statistics
                    info[from_pc].event[EVENT_BC]++;
                    // Simple misprediction model
                    if (branch.taken_count > 0 && branch.fallthrough_count > 0) {
                        uint64_t minority = std::min(branch.taken_count, branch.fallthrough_count);
                        if (minority == branch.taken_count || minority == branch.fallthrough_count) {
                            info[from_pc].event[EVENT_BCM]++;
                        }
                    }
                }
                break;
            }
            
            case BranchType::DIRECT_JUMP:
            case BranchType::INDIRECT_JUMP: {
                if (collect_jumps) {
                    jumps[from_pc][to_pc]++;  // Simple and fast
                    
                    if (type == BranchType::INDIRECT_JUMP) {
                        info[from_pc].event[EVENT_BI]++;
                        // Misprediction for indirect jumps with multiple targets
                        if (jumps[from_pc].size() > 1) {
                            info[from_pc].event[EVENT_BIM]++;
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
          num_events(2) {
        
        event_names = {"Ir", "Cycle", "Bc", "Bcm", "Bi", "Bim"};
        std::fill(std::begin(accumulated_events), std::end(accumulated_events), 0);
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
        }
        
        // Update events
        info[pc].event[event_type] += count;
        accumulated_events[event_type] += count;
        
        // Handle previous branch
        if (last_pc != 0 && last_was_branch) {
            bool is_sequential = (pc == last_pc + last_inst_size);
            BranchType branch_type = detectBranchType(last_pc, pc, last_dest_reg, is_sequential);
            handleBranch(last_pc, pc, branch_type, is_sequential);
        }
        
        // Update state
        last_pc = pc;
        last_dest_reg = dest_reg;
        last_was_branch = is_branch_instruction;
        
        if (it != info.end() && !it->second.assembly.empty()) {
            last_inst_size = detectInstructionSize(it->second.assembly);
        } else {
            last_inst_size = 4;
        }
    }
    
    // Write output
    void writeOutput() {
        std::ofstream out(output_filename);
        if (!out.is_open()) {
            std::cerr << "Failed to open output file: " << output_filename << std::endl;
            return;
        }
        
        // Header
        out << "# callgrind format\n";
        out << "version: 1\n";
        out << "creator: core-simulator\n";
        out << "pid: " << getpid() << "\n";
        out << "cmd: simulated_program\n";
        out << "part: 1\n\n";
        
        out << "positions:";
        if (dump_instr) out << " instr";
        out << " line\n";
        
        out << "events:";
        for (size_t i = 0; i < num_events && i < event_names.size(); ++i) {
            out << " " << event_names[i];
        }
        out << "\n\n";
        
        // Sort PCs
        std::vector<uint64_t> sorted_pcs;
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
            
            // Output calls
            auto call_it = calls.find(pc);
            if (call_it != calls.end()) {
                for (const auto& [target_pc, target_info] : call_it->second) {
                    auto callee_it = info.find(target_pc);
                    if (callee_it != info.end()) {
                        out << "cfn=" << callee_it->second.func << "\n";
                        out << "cfl=" << callee_it->second.file << "\n";
                    } else {
                        out << "cfn=unknown\n";
                    }
                    
                    out << "calls=" << target_info.count << " ";
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
                        out << " " << target_info.inclusive_events[i];
                    }
                    out << "\n";
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
                    // Output each target for this jump
                    for (const auto& [target_pc, count] : jump_it->second) {
                        auto target_it = info.find(target_pc);
                        
                        out << "jump=";
                        if (dump_instr) {
                            out << "0x" << std::hex << target_pc << std::dec;
                        }
                        out << "/" << (target_it != info.end() ? target_it->second.func : "unknown");
                        out << " " << count << "\n";
                    }
                }
            }
        }
        
        // Summary
        out << "\n# Summary\n";
        out << "totals:";
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

#endif // CALLGRIND_GENERATOR_SIMPLE_HPP