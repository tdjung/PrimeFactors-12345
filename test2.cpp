// callgrind_generator_final.hpp
#ifndef CALLGRIND_GENERATOR_FINAL_HPP
#define CALLGRIND_GENERATOR_FINAL_HPP

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
    BRANCH,           // Conditional branch (beq, bne, etc.)
    DIRECT_JUMP,      // Direct unconditional jump (j label)
    INDIRECT_JUMP,    // Indirect jump (jalr x0, rs)
    CALL,             // Function call (jal, jalr with link)
    RETURN,           // Function return
    TAIL_CALL         // Tail call optimization
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

// Call target information
struct CallTarget {
    uint64_t target_pc;
    uint64_t count;
    uint64_t inclusive_events[MAX_EVENTS];
    
    CallTarget() : target_pc(0), count(0) {
        std::fill(std::begin(inclusive_events), std::end(inclusive_events), 0);
    }
};

// Jump target information
struct JumpTarget {
    uint64_t target_pc;
    uint64_t executed;
    uint64_t taken;
    
    JumpTarget() : target_pc(0), executed(0), taken(0) {}
};

// Multiple targets from single PC
struct CallSite {
    std::vector<CallTarget> targets;
};

struct BranchSite {
    std::vector<JumpTarget> targets;
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
    
    // Call and jump tracking - PC as primary key for fast lookup
    std::unordered_map<uint64_t, CallSite> calls;   // from_pc -> multiple targets
    std::unordered_map<uint64_t, BranchSite> jumps; // from_pc -> multiple targets
    
    // Runtime state
    std::stack<CallStackEntry> call_stack;
    std::vector<std::pair<uint64_t, uint64_t>> tail_call_chain;
    
    // Branch tracking - handle multiple consecutive branches
    struct PendingBranch {
        uint64_t pc;
        int dest_reg;
        uint32_t inst_size;
    };
    std::vector<PendingBranch> pending_branches;  // Queue of unprocessed branches
    
    uint64_t last_pc;
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
        // Get function information
        auto from_it = info.find(from_pc);
        auto to_it = info.find(to_pc);
        
        if (from_it == info.end() || to_it == info.end()) {
            // For unknown code, assume conditional branch if not taken
            if (is_sequential) {
                return BranchType::BRANCH;
            }
            return BranchType::DIRECT_JUMP;
        }
        
        const std::string& from_func = from_it->second.func;
        const std::string& to_func = to_it->second.func;
        
        // Check for return - jumping back to caller's context
        if (!is_sequential && !call_stack.empty()) {
            const auto& stack_top = call_stack.top();
            auto caller_it = info.find(stack_top.caller_pc);
            if (caller_it != info.end()) {
                if (to_func == caller_it->second.func) {
                    return BranchType::RETURN;
                }
            }
        }
        
        // Check if jumping to different function (only when actually taken)
        if (!is_sequential && from_func != to_func) {
            // Function call or tail call
            if (dest_reg == 0) {
                return BranchType::TAIL_CALL;
            } else if (dest_reg > 0) {
                return BranchType::CALL;
            }
            return BranchType::CALL;
        }
        
        // Same function - determine branch/jump type
        
        // Sequential execution after branch instruction = not taken branch
        if (is_sequential) {
            return BranchType::BRANCH;  // Conditional branch, not taken
        }
        
        // Non-sequential execution - branch/jump was taken
        // Backward jumps are typically conditional branches (loops)
        if (to_pc < from_pc) {
            return BranchType::BRANCH;
        }
        
        // Forward jumps - analyze distance
        uint64_t jump_distance = to_pc - from_pc;
        
        // Small forward jumps are often conditional branches
        if (jump_distance <= 32) {
            return BranchType::BRANCH;
        }
        
        // Large forward jumps are typically direct jumps
        return BranchType::DIRECT_JUMP;
    }
    
    // Handle detected branch/call
    void handleBranch(uint64_t from_pc, uint64_t to_pc, BranchType type, bool is_sequential = false) {
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
                
                // Record call (NOT jump)
                auto& call_site = calls[from_pc];
                auto target_it = std::find_if(call_site.targets.begin(), 
                                              call_site.targets.end(),
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
                    
                    // Record tail call (NOT jump)
                    auto& call_site = calls[from_pc];
                    auto target_it = std::find_if(call_site.targets.begin(),
                                                  call_site.targets.end(),
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
                // Function return - calculate inclusive costs
                if (!call_stack.empty()) {
                    auto& entry = call_stack.top();
                    
                    // Calculate inclusive cost
                    uint64_t inclusive_cost[MAX_EVENTS];
                    for (size_t i = 0; i < MAX_EVENTS; ++i) {
                        inclusive_cost[i] = accumulated_events[i] - entry.events_at_entry[i];
                    }
                    
                    // Update call record - find the call site and target
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
                    
                    // Handle tail call chain costs
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
            
            case BranchType::BRANCH:
            case BranchType::DIRECT_JUMP:
            case BranchType::INDIRECT_JUMP: {
                if (collect_jumps) {
                    auto& branch_site = jumps[from_pc];
                    
                    // For branches, we need to track both taken and not-taken
                    if (type == BranchType::BRANCH) {
                        // Find or create target entry
                        auto jump_it = std::find_if(branch_site.targets.begin(),
                                                    branch_site.targets.end(),
                            [to_pc](const JumpTarget& t) { return t.target_pc == to_pc; });
                        
                        if (jump_it != branch_site.targets.end()) {
                            jump_it->executed++;
                            if (!is_sequential) {  // Taken only if not sequential
                                jump_it->taken++;
                            }
                        } else {
                            JumpTarget new_jump;
                            new_jump.target_pc = to_pc;
                            new_jump.executed = 1;
                            new_jump.taken = is_sequential ? 0 : 1;
                            branch_site.targets.push_back(new_jump);
                        }
                        
                        // Update branch statistics
                        info[from_pc].event[EVENT_BC]++;
                        // Simple misprediction model - if pattern changes
                        if (jump_it != branch_site.targets.end() &&
                            jump_it->taken != 0 && jump_it->taken != jump_it->executed) {
                            info[from_pc].event[EVENT_BCM]++;
                        }
                    } else {
                        // Direct/Indirect jumps are always taken
                        auto jump_it = std::find_if(branch_site.targets.begin(),
                                                    branch_site.targets.end(),
                            [to_pc](const JumpTarget& t) { return t.target_pc == to_pc; });
                        
                        if (jump_it != branch_site.targets.end()) {
                            jump_it->executed++;
                            jump_it->taken++;
                        } else {
                            JumpTarget new_jump;
                            new_jump.target_pc = to_pc;
                            new_jump.executed = 1;
                            new_jump.taken = 1;
                            branch_site.targets.push_back(new_jump);
                        }
                        
                        if (type == BranchType::INDIRECT_JUMP) {
                            info[from_pc].event[EVENT_BI]++;
                            if (branch_site.targets.size() > 1) {
                                info[from_pc].event[EVENT_BIM]++;
                            }
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
        
        // Process all pending branches
        // Check each pending branch to see if it was taken or not
        auto pending_it = pending_branches.begin();
        while (pending_it != pending_branches.end()) {
            uint64_t expected_next = pending_it->pc + pending_it->inst_size;
            bool is_sequential = (pc == expected_next);
            
            // Detect branch type and handle it
            BranchType branch_type = detectBranchType(pending_it->pc, pc, 
                                                      pending_it->dest_reg, is_sequential);
            handleBranch(pending_it->pc, pc, branch_type, is_sequential);
            
            // Remove processed branch
            pending_it = pending_branches.erase(pending_it);
            
            // If this was not sequential (branch taken), stop processing
            // as remaining branches weren't executed
            if (!is_sequential) {
                pending_branches.clear();
                break;
            }
        }
        
        // If current instruction is a branch, add to pending
        if (is_branch_instruction) {
            PendingBranch branch;
            branch.pc = pc;
            branch.dest_reg = dest_reg;
            
            // Detect instruction size
            if (it != info.end() && !it->second.assembly.empty()) {
                branch.inst_size = detectInstructionSize(it->second.assembly);
            } else {
                branch.inst_size = 4;  // Default
            }
            
            pending_branches.push_back(branch);
        }
        
        last_pc = pc;
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
            
            // Output events (self cost only)
            for (size_t i = 0; i < num_events; ++i) {
                out << " " << pc_info.event[i];
            }
            
            // Assembly comment
            if (dump_instr && !pc_info.assembly.empty()) {
                out << " # " << pc_info.assembly;
            }
            out << "\n";
            
            // Output call information inline if this PC is a call site
            auto call_it = calls.find(pc);
            if (call_it != calls.end()) {
                // Output each target for this call site
                for (const auto& target : call_it->second.targets) {
                    auto callee_it = info.find(target.target_pc);
                    if (callee_it != info.end()) {
                        out << "cfn=" << callee_it->second.func << "\n";
                        out << "cfl=" << callee_it->second.file << "\n";
                    } else {
                        out << "cfn=unknown\n";
                    }
                    
                    out << "calls=" << target.count << " ";
                    if (dump_instr) {
                        out << "0x" << std::hex << target.target_pc << std::dec;
                    }
                    out << " " << (callee_it != info.end() ? callee_it->second.line : 0) << "\n";
                    
                    // Output inclusive costs at call site
                    if (dump_instr) {
                        out << "0x" << std::hex << pc << std::dec;
                    }
                    out << " " << pc_info.line;
                    for (size_t i = 0; i < num_events; ++i) {
                        out << " " << target.inclusive_events[i];
                    }
                    out << "\n";
                }
            }
            
            // Output jumps inline
            if (collect_jumps) {
                auto jump_it = jumps.find(pc);
                if (jump_it != jumps.end()) {
                    // Output each jump target
                    for (const auto& jump : jump_it->second.targets) {
                        std::string target_fn = "unknown";
                        auto target_it = info.find(jump.target_pc);
                        if (target_it != info.end()) {
                            target_fn = target_it->second.func;
                        }
                        
                        // For conditional branches, check if it was taken
                        bool is_conditional = (jump_it->second.targets.size() > 1) || 
                                            (jump.taken < jump.executed);
                        
                        if (is_conditional) {
                            out << "jcnd=";  // Conditional jump
                        } else {
                            out << "jump=";  // Unconditional jump
                        }
                        
                        if (dump_instr) {
                            out << "0x" << std::hex << jump.target_pc << std::dec;
                        }
                        out << "/" << target_fn << " " << jump.taken << "\n";
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
    
    // Finalize and write output
    void finalize() {
        generator.writeOutput();
    }
};

#endif // CALLGRIND_GENERATOR_FINAL_HPP