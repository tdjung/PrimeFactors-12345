// callgrind_generator.hpp
#ifndef CALLGRIND_GENERATOR_HPP
#define CALLGRIND_GENERATOR_HPP

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
    // Add more as needed
};

// Instruction types for branch classification
enum class BranchType {
    NONE,
    CONDITIONAL_BRANCH,
    UNCONDITIONAL_BRANCH,
    CALL,
    RETURN,
    INDIRECT_JUMP,
    TAIL_CALL
};

// Per-PC information loaded from objdump
struct PCInfo {
    uint64_t pc;
    std::string func;
    std::string assembly;
    std::string file;
    uint32_t line;
    uint64_t event[MAX_EVENTS];  // Event counters
    
    // Additional fields for branch analysis
    BranchType branch_type;
    uint64_t target_pc;     // For direct branches/calls
    bool is_indirect;       // For indirect branches
    bool is_return;         // For return instructions
    
    PCInfo() : pc(0), line(0), branch_type(BranchType::NONE), 
               target_pc(0), is_indirect(false), is_return(false) {
        std::fill(std::begin(event), std::end(event), 0);
    }
};

// Call information
struct CallRecord {
    uint64_t caller_pc;
    uint64_t callee_pc;
    uint64_t count;
    uint64_t inclusive_events[MAX_EVENTS];  // Inclusive cost (callee + all its subcalls)
    
    CallRecord() : caller_pc(0), callee_pc(0), count(0) {
        std::fill(std::begin(inclusive_events), std::end(inclusive_events), 0);
    }
};

// Jump/Branch information
struct JumpRecord {
    uint64_t source_pc;
    uint64_t target_pc;
    uint64_t executed;
    uint64_t taken;
    
    JumpRecord() : source_pc(0), target_pc(0), executed(0), taken(0) {}
};

// Call stack entry for tracking function calls
struct CallStackEntry {
    uint64_t caller_pc;
    uint64_t callee_pc;
    std::string caller_func;
    std::string callee_func;
    uint64_t events_at_entry[MAX_EVENTS];  // Event counters when entering function
    bool is_tail_call;  // Track if this was a tail call
};

class CallgrindGenerator {
private:
    // Main data structure - matches your existing hash_map
    std::unordered_map<uint64_t, PCInfo> info;
    
    // Call and jump tracking
    std::map<std::pair<uint64_t, uint64_t>, CallRecord> calls;  // (caller_pc, callee_pc) -> record
    std::map<std::pair<uint64_t, uint64_t>, JumpRecord> jumps;  // (source_pc, target_pc) -> record
    
    // Runtime state for call tracking
    std::stack<CallStackEntry> call_stack;
    uint64_t last_pc;
    uint64_t accumulated_events[MAX_EVENTS];  // Running total of events
    
    // For tracking tail call chains
    std::vector<std::pair<uint64_t, uint64_t>> tail_call_chain;  // (from_pc, to_pc)
    
    // File mappings for compressed output
    std::unordered_map<std::string, uint32_t> file_id_map;
    std::unordered_map<std::string, uint32_t> fn_id_map;
    std::vector<std::string> file_names;
    std::vector<std::string> fn_names;
    
    // Output configuration
    std::string output_filename;
    bool dump_instr;
    bool branch_sim;
    bool collect_jumps;
    bool collect_systime;
    bool compress_strings;
    bool compress_pos;
    
    // Event configuration
    std::vector<std::string> event_names;
    size_t num_events;
    
    uint32_t getFileId(const std::string& filename) {
        if (filename.empty()) return 0;
        auto it = file_id_map.find(filename);
        if (it != file_id_map.end()) {
            return it->second;
        }
        uint32_t id = file_names.size() + 1;
        file_id_map[filename] = id;
        file_names.push_back(filename);
        return id;
    }
    
    uint32_t getFnId(const std::string& fnname) {
        if (fnname.empty()) return 0;
        auto it = fn_id_map.find(fnname);
        if (it != fn_id_map.end()) {
            return it->second;
        }
        uint32_t id = fn_names.size() + 1;
        fn_id_map[fnname] = id;
        fn_names.push_back(fnname);
        return id;
    }
    
    // Analyze instruction to determine branch type
    void analyzeBranchType(PCInfo& pc_info) {
        const std::string& asm_str = pc_info.assembly;
        
        // RISC-V examples - adjust for your ISA
        if (asm_str.find("jal") != std::string::npos) {
            if (asm_str.find("jal\tx0") != std::string::npos || 
                asm_str.find("jal\tzero") != std::string::npos) {
                pc_info.branch_type = BranchType::TAIL_CALL;
            } else {
                pc_info.branch_type = BranchType::CALL;
            }
        } else if (asm_str.find("jalr") != std::string::npos) {
            if (asm_str.find("jalr\tx0") != std::string::npos ||
                asm_str.find("jalr\tzero") != std::string::npos) {
                if (asm_str.find("ra") != std::string::npos || 
                    asm_str.find("x1") != std::string::npos) {
                    pc_info.branch_type = BranchType::RETURN;
                    pc_info.is_return = true;
                } else {
                    pc_info.branch_type = BranchType::TAIL_CALL;
                }
            } else {
                pc_info.branch_type = BranchType::CALL;
                pc_info.is_indirect = true;
            }
        } else if (asm_str.find("beq") != std::string::npos ||
                   asm_str.find("bne") != std::string::npos ||
                   asm_str.find("blt") != std::string::npos ||
                   asm_str.find("bge") != std::string::npos ||
                   asm_str.find("bltu") != std::string::npos ||
                   asm_str.find("bgeu") != std::string::npos) {
            pc_info.branch_type = BranchType::CONDITIONAL_BRANCH;
        } else if (asm_str.find("j\t") != std::string::npos) {
            pc_info.branch_type = BranchType::UNCONDITIONAL_BRANCH;
        } else if (asm_str.find("ret") != std::string::npos) {
            pc_info.branch_type = BranchType::RETURN;
            pc_info.is_return = true;
        }
        
        // Try to extract target address for direct branches
        // This is a simplified parser - enhance based on your objdump format
        size_t hex_pos = asm_str.find("0x");
        if (hex_pos != std::string::npos && !pc_info.is_indirect) {
            std::stringstream ss(asm_str.substr(hex_pos));
            ss >> std::hex >> pc_info.target_pc;
        }
    }
    
public:
    CallgrindGenerator(const std::string& filename = "callgrind.out") 
        : output_filename(filename),
          last_pc(0),
          dump_instr(true),
          branch_sim(true),
          collect_jumps(true),
          collect_systime(true),
          compress_strings(false),
          compress_pos(false),
          num_events(2) {
        
        // Default events
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
    
    // Load objdump information into hash_map
    void loadPCInfo(uint64_t pc, const std::string& func, 
                    const std::string& assembly, const std::string& file, 
                    uint32_t line) {
        PCInfo& pc_info = info[pc];
        pc_info.pc = pc;
        pc_info.func = func;
        pc_info.assembly = assembly;
        pc_info.file = file;
        pc_info.line = line;
        
        // Analyze instruction for branch type
        analyzeBranchType(pc_info);
        
        // Pre-register file and function names if using compression
        if (compress_strings) {
            getFileId(file);
            getFnId(func);
        }
    }
    
    // Main simulation interface - called for each instruction execution
    void recordExecution(uint64_t pc, EventType event_type, uint64_t count) {
        auto it = info.find(pc);
        if (it == info.end()) {
            // PC not in objdump data - create minimal entry
            PCInfo& pc_info = info[pc];
            pc_info.pc = pc;
            pc_info.func = "unknown";
            pc_info.file = "unknown";
            pc_info.line = 0;
        }
        
        // Update event counter
        info[pc].event[event_type] += count;
        
        // Update accumulated events
        accumulated_events[event_type] += count;
        
        // Handle call/return/jump tracking
        handleControlFlow(pc);
        
        last_pc = pc;
    }
    
    // Alternative interface that accepts multiple events at once
    void recordExecutionMulti(uint64_t pc, const uint64_t* events, size_t event_count) {
        for (size_t i = 0; i < event_count && i < MAX_EVENTS; ++i) {
            if (events[i] > 0) {
                info[pc].event[i] += events[i];
                accumulated_events[i] += events[i];
            }
        }
        
        handleControlFlow(pc);
        last_pc = pc;
    }
    
    // Handle control flow tracking
    void handleControlFlow(uint64_t pc) {
        if (last_pc == 0) {
            last_pc = pc;
            return;
        }
        
        auto& last_info = info[last_pc];
        auto& curr_info = info[pc];
        
        // Check if last instruction was a branch/call
        switch (last_info.branch_type) {
            case BranchType::CALL: {
                // Regular function call
                uint64_t callee_pc = (last_info.target_pc != 0) ? last_info.target_pc : pc;
                
                // Create call stack entry
                CallStackEntry entry;
                entry.caller_pc = last_pc;
                entry.callee_pc = callee_pc;
                entry.caller_func = last_info.func;
                entry.callee_func = (info.find(callee_pc) != info.end()) ? 
                                    info[callee_pc].func : "unknown";
                entry.is_tail_call = false;
                
                // Save current event counters
                std::copy(std::begin(accumulated_events), 
                         std::end(accumulated_events), 
                         std::begin(entry.events_at_entry));
                
                call_stack.push(entry);
                
                // Initialize call record (will be updated on return)
                auto& call = calls[{last_pc, callee_pc}];
                call.caller_pc = last_pc;
                call.callee_pc = callee_pc;
                call.count++;
                
                // Track as jump if collecting jumps
                if (collect_jumps) {
                    auto& jump = jumps[{last_pc, callee_pc}];
                    jump.source_pc = last_pc;
                    jump.target_pc = callee_pc;
                    jump.executed++;
                    jump.taken++;
                }
                break;
            }
            
            case BranchType::TAIL_CALL: {
                // Tail call - doesn't return to caller
                uint64_t callee_pc = (last_info.target_pc != 0) ? last_info.target_pc : pc;
                
                // Record the tail call
                tail_call_chain.push_back({last_pc, callee_pc});
                
                // For tail calls, we need to attribute costs to the original caller
                if (!call_stack.empty()) {
                    // Get the original caller from stack
                    auto& original_entry = call_stack.top();
                    
                    // Create a modified entry for the tail call
                    CallStackEntry tail_entry;
                    tail_entry.caller_pc = original_entry.caller_pc;  // Original caller
                    tail_entry.callee_pc = callee_pc;  // New callee
                    tail_entry.caller_func = original_entry.caller_func;
                    tail_entry.callee_func = (info.find(callee_pc) != info.end()) ? 
                                            info[callee_pc].func : "unknown";
                    tail_entry.is_tail_call = true;
                    
                    // Keep the same entry events (from original call)
                    std::copy(std::begin(original_entry.events_at_entry),
                             std::end(original_entry.events_at_entry),
                             std::begin(tail_entry.events_at_entry));
                    
                    // Replace the stack top with tail call entry
                    call_stack.pop();
                    call_stack.push(tail_entry);
                    
                    // Record the tail call
                    auto& call = calls[{last_pc, callee_pc}];
                    call.caller_pc = last_pc;
                    call.callee_pc = callee_pc;
                    call.count++;
                }
                
                if (collect_jumps) {
                    auto& jump = jumps[{last_pc, callee_pc}];
                    jump.source_pc = last_pc;
                    jump.target_pc = callee_pc;
                    jump.executed++;
                    jump.taken++;
                }
                break;
            }
            
            case BranchType::RETURN: {
                // Function return - need to update inclusive costs
                if (!call_stack.empty()) {
                    auto& entry = call_stack.top();
                    
                    // Calculate inclusive cost (events since function entry)
                    uint64_t inclusive_cost[MAX_EVENTS];
                    for (size_t i = 0; i < MAX_EVENTS; ++i) {
                        inclusive_cost[i] = accumulated_events[i] - entry.events_at_entry[i];
                    }
                    
                    // Update the call record with inclusive costs
                    auto call_key = std::make_pair(entry.caller_pc, entry.callee_pc);
                    auto call_it = calls.find(call_key);
                    if (call_it != calls.end()) {
                        for (size_t i = 0; i < MAX_EVENTS; ++i) {
                            call_it->second.inclusive_events[i] += inclusive_cost[i];
                        }
                    }
                    
                    // If this was a tail call chain, also update intermediate tail calls
                    if (entry.is_tail_call && !tail_call_chain.empty()) {
                        // Attribute costs to all tail calls in the chain
                        for (const auto& [tail_from, tail_to] : tail_call_chain) {
                            auto tail_call_it = calls.find({tail_from, tail_to});
                            if (tail_call_it != calls.end()) {
                                for (size_t i = 0; i < MAX_EVENTS; ++i) {
                                    // Distribute inclusive cost proportionally
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
            
            case BranchType::CONDITIONAL_BRANCH: {
                // Check if branch was taken
                uint64_t expected_next = last_pc + 4;  // Adjust for your ISA
                bool taken = (pc != expected_next);
                
                if (collect_jumps) {
                    uint64_t target = taken ? pc : expected_next;
                    auto& jump = jumps[{last_pc, target}];
                    jump.source_pc = last_pc;
                    jump.target_pc = target;
                    jump.executed++;
                    if (taken) jump.taken++;
                    
                    // Update branch statistics
                    info[last_pc].event[EVENT_BC]++;
                    if (taken != (jump.taken > jump.executed / 2)) {
                        info[last_pc].event[EVENT_BCM]++;  // Simple misprediction model
                    }
                }
                break;
            }
            
            case BranchType::UNCONDITIONAL_BRANCH: {
                if (collect_jumps) {
                    uint64_t target = (last_info.target_pc != 0) ? last_info.target_pc : pc;
                    auto& jump = jumps[{last_pc, target}];
                    jump.source_pc = last_pc;
                    jump.target_pc = target;
                    jump.executed++;
                    jump.taken++;
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    // Write output in callgrind format
    void writeOutput() {
        std::ofstream out(output_filename);
        if (!out.is_open()) {
            std::cerr << "Failed to open output file: " << output_filename << std::endl;
            return;
        }
        
        // Write header
        out << "# callgrind format\n";
        out << "version: 1\n";
        out << "creator: core-simulator\n";
        out << "pid: " << getpid() << "\n";
        out << "cmd: simulated_program\n";
        out << "part: 1\n\n";
        
        // Write positions and events
        out << "positions:";
        if (dump_instr) out << " instr";
        out << " line\n";
        
        out << "events:";
        for (size_t i = 0; i < num_events && i < event_names.size(); ++i) {
            out << " " << event_names[i];
        }
        out << "\n\n";
        
        // Write compressed string mappings if enabled
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
        
        // Write instruction costs grouped by function
        std::string current_func;
        std::string current_file;
        uint32_t last_line = 0;
        
        for (uint64_t pc : sorted_pcs) {
            const PCInfo& pc_info = info[pc];
            
            // Skip if no events recorded
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
            
            // Output line number
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
            
            // Output event counts
            for (size_t i = 0; i < num_events; ++i) {
                out << " " << pc_info.event[i];
            }
            
            // Add assembly comment if enabled
            if (dump_instr && !pc_info.assembly.empty()) {
                out << " # " << pc_info.assembly;
            }
            out << "\n";
            
            // Output jump information if this is a branch
            if (collect_jumps && pc_info.branch_type != BranchType::NONE) {
                for (const auto& [key, jump] : jumps) {
                    if (key.first == pc) {
                        // Find target function
                        std::string target_fn = "unknown";
                        auto target_it = info.find(key.second);
                        if (target_it != info.end()) {
                            target_fn = target_it->second.func;
                        }
                        
                        if (pc_info.branch_type == BranchType::CONDITIONAL_BRANCH) {
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
                
                // Output inclusive cost at call site
                // This is the cost of the callee + all its recursive calls
                if (dump_instr) {
                    out << "0x" << std::hex << key.first << std::dec;
                }
                out << " " << caller_it->second.line;
                
                // Output all inclusive event costs
                for (size_t i = 0; i < num_events; ++i) {
                    out << " " << call.inclusive_events[i];
                }
                out << "\n\n";
            }
        }
        
        // Write summary
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

// Example usage wrapper for your simulator
class SimulatorInterface {
private:
    CallgrindGenerator generator;
    
public:
    SimulatorInterface(const std::string& output_file = "callgrind.out.sim") 
        : generator(output_file) {
        
        // Configure with your options
        generator.setOptions(true, true, true, false, false);
        
        // Configure events - match your simulator's events
        generator.configureEvents({"Ir", "Cycle", "Bc", "Bcm", "Bi", "Bim"});
    }
    
    // Called once to load all objdump data
    void loadObjdumpData(const std::vector<std::tuple<uint64_t, std::string, std::string, std::string, uint32_t>>& objdump_data) {
        for (const auto& [pc, func, assembly, file, line] : objdump_data) {
            generator.loadPCInfo(pc, func, assembly, file, line);
        }
    }
    
    // Called during simulation - simple interface
    void onInstruction(uint64_t pc, EventType event, uint64_t count) {
        generator.recordExecution(pc, event, count);
    }
    
    // Called during simulation - batch interface
    void onInstructionBatch(uint64_t pc, const uint64_t* events, size_t event_count) {
        generator.recordExecutionMulti(pc, events, event_count);
    }
    
    // Called at end of simulation
    void finalize() {
        generator.writeOutput();
    }
};

#endif // CALLGRIND_GENERATOR_HPP