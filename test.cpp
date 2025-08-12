// callgrind_generator_clean.hpp
#ifndef CALLGRIND_GENERATOR_CLEAN_HPP
#define CALLGRIND_GENERATOR_CLEAN_HPP

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <stack>
#include <cstdint>
#include <unistd.h>

// Maximum number of events to track
constexpr size_t MAX_EVENTS = 10;

// Event types that can be tracked
enum EventType {
    EVENT_IR = 0,      // Instruction count
    EVENT_CYCLE = 1,   // Cycle count
    EVENT_BC = 2,      // Conditional branches
    EVENT_BCM = 3,     // Conditional branch mispredictions
    EVENT_BI = 4,      // Indirect branches
    EVENT_BIM = 5,     // Indirect branch mispredictions
    EVENT_CACHE_MISS = 6,  // Cache misses (optional)
    EVENT_TLB_MISS = 7,    // TLB misses (optional)
};

// Branch types detected at runtime
enum class BranchType {
    NONE,
    CONDITIONAL_BRANCH,
    UNCONDITIONAL_BRANCH,
    CALL,
    RETURN,
    TAIL_CALL
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

// Call record with inclusive costs
struct CallRecord {
    uint64_t caller_pc;
    uint64_t callee_pc;
    uint64_t count;
    uint64_t inclusive_events[MAX_EVENTS];
    
    CallRecord() : caller_pc(0), callee_pc(0), count(0) {
        std::fill(std::begin(inclusive_events), std::end(inclusive_events), 0);
    }
};

// Jump/Branch record
struct JumpRecord {
    uint64_t source_pc;
    uint64_t target_pc;
    uint64_t executed;
    uint64_t taken;
    
    JumpRecord() : source_pc(0), target_pc(0), executed(0), taken(0) {}
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
    // Main data structure
    std::unordered_map<uint64_t, PCInfo> info;
    
    // Call and jump tracking
    std::map<std::pair<uint64_t, uint64_t>, CallRecord> calls;
    std::map<std::pair<uint64_t, uint64_t>, JumpRecord> jumps;
    
    // Runtime state
    std::stack<CallStackEntry> call_stack;
    std::vector<std::pair<uint64_t, uint64_t>> tail_call_chain;
    uint64_t last_pc;
    int last_dest_reg;
    bool last_was_branch;
    uint32_t last_inst_size;
    uint64_t accumulated_events[MAX_EVENTS];
    
    // String compression for output
    std::unordered_map<std::string, uint32_t> file_id_map;
    std::unordered_map<std::string, uint32_t> fn_id_map;
    std::vector<std::string> file_names;
    std::vector<std::string> fn_names;
    
    // Configuration
    std::string output_filename;
    bool dump_instr;
    bool branch_sim;
    bool collect_jumps;
    bool compress_strings;
    bool compress_pos;
    
    // Event configuration
    std::vector<std::string> event_names;
    size_t num_events;
    
    uint32_t getFileId(const std::string& filename) {
        if (filename.empty()) return 0;
        auto it = file_id_map.find(filename);
        if (it != file_id_map.end()) return it->second;
        uint32_t id = file_names.size() + 1;
        file_id_map[filename] = id;
        file_names.push_back(filename);
        return id;
    }
    
    uint32_t getFnId(const std::string& fnname) {
        if (fnname.empty()) return 0;
        auto it = fn_id_map.find(fnname);
        if (it != fn_id_map.end()) return it->second;
        uint32_t id = fn_names.size() + 1;
        fn_id_map[fnname] = id;
        fn_names.push_back(fnname);
        return id;
    }
    
    // Detect instruction size from assembly (for RISC-V compressed instructions)
    uint32_t detectInstructionSize(const std::string& assembly) {
        // RISC-V compressed instructions typically start with 'c.'
        if (assembly.find("c.") != std::string::npos || 
            assembly.find("\tc.") != std::string::npos) {
            return 2;
        }
        return 4;  // Default for normal instructions
    }
    
    // Runtime branch type detection based on execution flow
    BranchType detectBranchType(uint64_t from_pc, uint64_t to_pc, int dest_reg, bool is_sequential) {
        // Sequential execution means no branch taken
        if (is_sequential) {
            return BranchType::NONE;
        }
        
        // Get function information
        auto from_it = info.find(from_pc);
        auto to_it = info.find(to_pc);
        
        if (from_it == info.end() || to_it == info.end()) {
            return BranchType::UNCONDITIONAL_BRANCH;
        }
        
        const std::string& from_func = from_it->second.func;
        const std::string& to_func = to_it->second.func;
        
        // Check for return - jumping back to caller's context
        if (!call_stack.empty()) {
            const auto& stack_top = call_stack.top();
            auto caller_it = info.find(stack_top.caller_pc);
            if (caller_it != info.end()) {
                // Check if returning to caller's function
                if (to_func == caller_it->second.func) {
                    return BranchType::RETURN;
                }
            }
        }
        
        // Jumping to different function indicates call or tail call
        if (from_func != to_func) {
            // dest_reg determines if it's a call or tail call
            // RISC-V: x0 (reg 0) means no return address saved
            if (dest_reg == 0) {
                return BranchType::TAIL_CALL;
            } else if (dest_reg > 0) {
                return BranchType::CALL;
            }
            // Unknown dest_reg: assume regular call
            return BranchType::CALL;
        }
        
        // Same function jump - determine if conditional or unconditional
        if (to_pc < from_pc) {
            // Backward jump - typically loop (conditional)
            return BranchType::CONDITIONAL_BRANCH;
        } else {
            // Forward jump - check distance
            uint64_t jump_distance = to_pc - from_pc;
            if (jump_distance > 32) {
                return BranchType::UNCONDITIONAL_BRANCH;
            }
            return BranchType::CONDITIONAL_BRANCH;
        }
    }
    
    // Handle detected branch/call
    void handleBranch(uint64_t from_pc, uint64_t to_pc, BranchType type) {
        switch (type) {
            case BranchType::CALL: {
                // Regular function call - push to stack
                CallStackEntry entry;
                entry.caller_pc = from_pc;
                entry.callee_pc = to_pc;
                entry.caller_func = info[from_pc].func;
                entry.callee_func = info[to_pc].func;
                entry.is_tail_call = false;
                
                // Save event state at entry
                std::copy(std::begin(accumulated_events), 
                         std::end(accumulated_events), 
                         std::begin(entry.events_at_entry));
                
                call_stack.push(entry);
                
                // Record call
                auto& call = calls[{from_pc, to_pc}];
                call.caller_pc = from_pc;
                call.callee_pc = to_pc;
                call.count++;
                
                // Track as jump if enabled
                if (collect_jumps) {
                    auto& jump = jumps[{from_pc, to_pc}];
                    jump.source_pc = from_pc;
                    jump.target_pc = to_pc;
                    jump.executed++;
                    jump.taken++;
                }
                break;
            }
            
            case BranchType::TAIL_CALL: {
                // Tail call - replace stack top
                tail_call_chain.push_back({from_pc, to_pc});
                
                if (!call_stack.empty()) {
                    auto& original_entry = call_stack.top();
                    
                    // Create new entry maintaining original caller
                    CallStackEntry tail_entry;
                    tail_entry.caller_pc = original_entry.caller_pc;  // Keep original
                    tail_entry.callee_pc = to_pc;  // New target
                    tail_entry.caller_func = original_entry.caller_func;
                    tail_entry.callee_func = info[to_pc].func;
                    tail_entry.is_tail_call = true;
                    
                    // Keep original entry events
                    std::copy(std::begin(original_entry.events_at_entry),
                             std::end(original_entry.events_at_entry),
                             std::begin(tail_entry.events_at_entry));
                    
                    // Replace stack top
                    call_stack.pop();
                    call_stack.push(tail_entry);
                    
                    // Record tail call
                    auto& call = calls[{from_pc, to_pc}];
                    call.caller_pc = from_pc;
                    call.callee_pc = to_pc;
                    call.count++;
                }
                
                if (collect_jumps) {
                    auto& jump = jumps[{from_pc, to_pc}];
                    jump.source_pc = from_pc;
                    jump.target_pc = to_pc;
                    jump.executed++;
                    jump.taken++;
                }
                break;
            }
            
            case BranchType::RETURN: {
                // Function return - calculate inclusive costs
                if (!call_stack.empty()) {
                    auto& entry = call_stack.top();
                    
                    // Calculate inclusive cost
                    uint64_t inclusive_cost[MAX_EVENTS];
                    for (size_t i = 0; i < MAX_EVENTS; ++i) {
                        inclusive_cost[i] = accumulated_events[i] - entry.events_at_entry[i];
                    }
                    
                    // Update call record
                    auto call_key = std::make_pair(entry.caller_pc, entry.callee_pc);
                    auto call_it = calls.find(call_key);
                    if (call_it != calls.end()) {
                        for (size_t i = 0; i < MAX_EVENTS; ++i) {
                            call_it->second.inclusive_events[i] += inclusive_cost[i];
                        }
                    }
                    
                    // Handle tail call chain costs
                    if (entry.is_tail_call && !tail_call_chain.empty()) {
                        for (const auto& [tail_from, tail_to] : tail_call_chain) {
                            auto tail_call_it = calls.find({tail_from, tail_to});
                            if (tail_call_it != calls.end()) {
                                for (size_t i = 0; i < MAX_EVENTS; ++i) {
                                    tail_call_it->second.inclusive_events[i] += 
                                        inclusive_cost[i] / (tail_call_chain.size() + 1);
                                }
                            }
                        }
                        tail_call_chain.clear();
                    }
                    
                    call_stack.pop();
                }
                break;
            }
            
            case BranchType::CONDITIONAL_BRANCH:
            case BranchType::UNCONDITIONAL_BRANCH: {
                if (collect_jumps) {
                    auto& jump = jumps[{from_pc, to_pc}];
                    jump.source_pc = from_pc;
                    jump.target_pc = to_pc;
                    jump.executed++;
                    jump.taken++;
                    
                    // Update branch statistics
                    if (type == BranchType::CONDITIONAL_BRANCH) {
                        info[from_pc].event[EVENT_BC]++;
                        // Simple misprediction model
                        if (jump.taken <= jump.executed / 2) {
                            info[from_pc].event[EVENT_BCM]++;
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
          compress_strings(false),
          compress_pos(false),
          num_events(2) {
        
        event_names = {"Ir", "Cycle", "Bc", "Bcm", "Bi", "Bim"};
        std::fill(std::begin(accumulated_events), std::end(accumulated_events), 0);
    }
    
    void setOptions(bool dump_instr_opt, bool branch_sim_opt, 
                    bool collect_jumps_opt, bool compress_strings_opt,
                    bool compress_pos_opt) {
        dump_instr = dump_instr_opt;
        branch_sim = branch_sim_opt;
        collect_jumps = collect_jumps_opt;
        compress_strings = compress_strings_opt;
        compress_pos = compress_pos_opt;
    }
    
    void configureEvents(const std::vector<std::string>& names) {
        event_names = names;
        num_events = names.size();
    }
    
    // Load objdump data - no analysis, just store
    void loadPCInfo(uint64_t pc, const std::string& func, 
                    const std::string& assembly, const std::string& file, 
                    uint32_t line) {
        PCInfo& pc_info = info[pc];
        pc_info.pc = pc;
        pc_info.func = func;
        pc_info.assembly = assembly;
        pc_info.file = file;
        pc_info.line = line;
        
        if (compress_strings) {
            getFileId(file);
            getFnId(func);
        }
    }
    
    // Main recording interface
    void recordExecution(uint64_t pc, EventType event_type, uint64_t count, 
                        int dest_reg = -1, bool is_branch_instruction = false) {
        // Create entry if doesn't exist
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
        
        // Check if previous instruction was a branch
        if (last_pc != 0 && last_was_branch) {
            bool is_sequential = (pc == last_pc + last_inst_size);
            BranchType branch_type = detectBranchType(last_pc, pc, last_dest_reg, is_sequential);
            if (branch_type != BranchType::NONE) {
                handleBranch(last_pc, pc, branch_type);
            }
        }
        
        // Update state for next iteration
        last_pc = pc;
        last_dest_reg = dest_reg;
        last_was_branch = is_branch_instruction;
        
        // Detect instruction size
        if (it != info.end() && !it->second.assembly.empty()) {
            last_inst_size = detectInstructionSize(it->second.assembly);
        } else {
            last_inst_size = 4;  // Default
        }
    }
    
    // Batch recording interface
    void recordExecutionMulti(uint64_t pc, const uint64_t* events, size_t event_count, 
                             int dest_reg = -1, bool is_branch_instruction = false) {
        // Update all events
        for (size_t i = 0; i < event_count && i < MAX_EVENTS; ++i) {
            if (events[i] > 0) {
                info[pc].event[i] += events[i];
                accumulated_events[i] += events[i];
            }
        }
        
        // Handle control flow
        if (last_pc != 0 && last_was_branch) {
            bool is_sequential = (pc == last_pc + last_inst_size);
            BranchType branch_type = detectBranchType(last_pc, pc, last_dest_reg, is_sequential);
            if (branch_type != BranchType::NONE) {
                handleBranch(last_pc, pc, branch_type);
            }
        }
        
        last_pc = pc;
        last_dest_reg = dest_reg;
        last_was_branch = is_branch_instruction;
        
        auto it = info.find(pc);
        if (it != info.end() && !it->second.assembly.empty()) {
            last_inst_size = detectInstructionSize(it->second.assembly);
        } else {
            last_inst_size = 4;
        }
    }
    
    // Write callgrind format output
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
        
        // Positions and events
        out << "positions:";
        if (dump_instr) out << " instr";
        out << " line\n";
        
        out << "events:";
        for (size_t i = 0; i < num_events && i < event_names.size(); ++i) {
            out << " " << event_names[i];
        }
        out << "\n\n";
        
        // String mappings if compressed
        if (compress_strings) {
            for (size_t i = 0; i < file_names.size(); ++i) {
                out << "fl=(" << (i + 1) << ") " << file_names[i] << "\n";
            }
            out << "\n";
            
            for (size_t i = 0; i < fn_names.size(); ++i) {
                out << "fn=(" << (i + 1) << ") " << fn_names[i] << "\n";
            }
            out << "\n";
        }
        
        // Sort PCs for output
        std::vector<uint64_t> sorted_pcs;
        for (const auto& [pc, _] : info) {
            sorted_pcs.push_back(pc);
        }
        std::sort(sorted_pcs.begin(), sorted_pcs.end());
        
        // Write instruction costs
        std::string current_func;
        std::string current_file;
        uint32_t last_line = 0;
        
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
            
            // Output function change
            if (pc_info.func != current_func) {
                current_func = pc_info.func;
                if (compress_strings) {
                    out << "fn=(" << getFnId(current_func) << ")\n";
                } else {
                    out << "fn=" << current_func << "\n";
                }
                last_line = 0;
            }
            
            // Output file change
            if (pc_info.file != current_file) {
                current_file = pc_info.file;
                if (compress_strings) {
                    out << "fl=(" << getFileId(current_file) << ")\n";
                } else {
                    out << "fl=" << current_file << "\n";
                }
                last_line = 0;
            }
            
            // Output position
            if (dump_instr) {
                out << "0x" << std::hex << pc << std::dec;
            }
            
            // Output line
            if (compress_pos && last_line != 0) {
                int32_t diff = pc_info.line - last_line;
                if (diff >= 0) {
                    out << " +" << diff;
                } else {
                    out << " " << diff;
                }
            } else {
                out << " " << pc_info.line;
            }
            last_line = pc_info.line;
            
            // Output events
            for (size_t i = 0; i < num_events; ++i) {
                out << " " << pc_info.event[i];
            }
            
            // Assembly comment
            if (dump_instr && !pc_info.assembly.empty()) {
                out << " # " << pc_info.assembly;
            }
            out << "\n";
            
            // Output jumps
            if (collect_jumps) {
                for (const auto& [key, jump] : jumps) {
                    if (key.first == pc) {
                        std::string target_fn = "unknown";
                        auto target_it = info.find(key.second);
                        if (target_it != info.end()) {
                            target_fn = target_it->second.func;
                        }
                        
                        // Determine jump type from recorded data
                        bool is_conditional = (jump.taken < jump.executed);
                        
                        if (is_conditional) {
                            out << "jcnd=";
                        } else {
                            out << "jump=";
                        }
                        
                        if (dump_instr) {
                            out << "0x" << std::hex << key.second << std::dec;
                        }
                        out << "/" << target_fn << " " << jump.taken << "\n";
                    }
                }
            }
        }
        
        // Write call graph
        out << "\n# Call graph\n";
        for (const auto& [key, call] : calls) {
            auto caller_it = info.find(key.first);
            auto callee_it = info.find(key.second);
            
            if (caller_it != info.end()) {
                out << "fn=" << caller_it->second.func << "\n";
                out << "fl=" << caller_it->second.file << "\n";
                
                if (callee_it != info.end()) {
                    out << "cfn=" << callee_it->second.func << "\n";
                    out << "cfl=" << callee_it->second.file << "\n";
                }
                
                out << "calls=" << call.count << " ";
                if (dump_instr) {
                    out << "0x" << std::hex << key.second << std::dec;
                }
                out << " " << (callee_it != info.end() ? callee_it->second.line : 0) << "\n";
                
                // Output inclusive costs
                if (dump_instr) {
                    out << "0x" << std::hex << key.first << std::dec;
                }
                out << " " << caller_it->second.line;
                
                for (size_t i = 0; i < num_events; ++i) {
                    out << " " << call.inclusive_events[i];
                }
                out << "\n\n";
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

// Simulator interface wrapper
class SimulatorInterface {
private:
    CallgrindGenerator generator;
    
public:
    SimulatorInterface(const std::string& output_file = "callgrind.out.sim") 
        : generator(output_file) {
        
        generator.setOptions(true, true, true, false, false);
        generator.configureEvents({"Ir", "Cycle", "Bc", "Bcm", "Bi", "Bim"});
    }
    
    // Load objdump data
    void loadObjdumpData(const std::vector<std::tuple<uint64_t, std::string, std::string, std::string, uint32_t>>& objdump_data) {
        for (const auto& [pc, func, assembly, file, line] : objdump_data) {
            generator.loadPCInfo(pc, func, assembly, file, line);
        }
    }
    
    // Record instruction execution
    // dest_reg: -1=unknown, 0=x0/zero (tail call), >0=link register (call)
    // is_branch: true if instruction can change control flow
    void onInstruction(uint64_t pc, EventType event, uint64_t count, 
                      int dest_reg = -1, bool is_branch = false) {
        generator.recordExecution(pc, event, count, dest_reg, is_branch);
    }
    
    // Batch recording
    void onInstructionBatch(uint64_t pc, const uint64_t* events, size_t event_count,
                           int dest_reg = -1, bool is_branch = false) {
        generator.recordExecutionMulti(pc, events, event_count, dest_reg, is_branch);
    }
    
    // Finalize and write output
    void finalize() {
        generator.writeOutput();
    }
};

// Usage example
/*
int main() {
    SimulatorInterface sim("output.callgrind");
    
    // Load symbol information from objdump
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string, uint32_t>> objdump_data = {
        {0x1000, "main", "addi sp,sp,-16", "main.c", 10},
        {0x1004, "main", "jal ra,0x2000", "main.c", 11},
        {0x1008, "main", "addi sp,sp,16", "main.c", 12},
        {0x100c, "main", "ret", "main.c", 13},
        {0x2000, "func1", "addi a0,a0,1", "func.c", 20},
        {0x2004, "func1", "j 0x3000", "func.c", 21},  // tail call
        {0x3000, "func2", "addi a0,a0,2", "func.c", 30},
        {0x3004, "func2", "ret", "func.c", 31},
    };
    sim.loadObjdumpData(objdump_data);
    
    // Simulate execution
    sim.onInstruction(0x1000, EVENT_IR, 1, -1, false);  // addi
    sim.onInstruction(0x1004, EVENT_IR, 1, 1, true);    // jal ra (call)
    sim.onInstruction(0x2000, EVENT_IR, 1, -1, false);  // addi in func1
    sim.onInstruction(0x2004, EVENT_IR, 1, 0, true);    // j (tail call with x0)
    sim.onInstruction(0x3000, EVENT_IR, 1, -1, false);  // addi in func2
    sim.onInstruction(0x3004, EVENT_IR, 1, -1, true);   // ret
    sim.onInstruction(0x1008, EVENT_IR, 1, -1, false);  // back to main
    sim.onInstruction(0x100c, EVENT_IR, 1, -1, true);   // ret from main
    
    sim.finalize();
    return 0;
}
*/

#endif // CALLGRIND_GENERATOR_CLEAN_HPP