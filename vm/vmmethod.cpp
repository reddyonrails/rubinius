#include "arguments.hpp"
#include "dispatch.hpp"
#include "call_frame.hpp"
#include "objectmemory.hpp"
#include "prelude.hpp"
#include "vmmethod.hpp"

#include "vm/object_utils.hpp"

#include "builtin/array.hpp"
#include "builtin/compiledmethod.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/iseq.hpp"
#include "builtin/symbol.hpp"
#include "builtin/tuple.hpp"
#include "builtin/class.hpp"
#include "builtin/system.hpp"
#include "instructions.hpp"

#include "instruments/profiler.hpp"
#include "instruments/timing.hpp"
#include "config.h"

#include "raise_reason.hpp"
#include "inline_cache.hpp"

#include "configuration.hpp"

#ifdef ENABLE_LLVM
#include "llvm/jit.hpp"
#endif

#define USE_SPECIALIZED_EXECUTE

/*
 * An internalization of a CompiledMethod which holds the instructions for the
 * method.
 */
namespace rubinius {

  /** System-wide standard interpreter method pointer. */
  static InterpreterRunner standard_interpreter = 0;

  /** System-wide dynamic interpreter method pointer. */
  static InterpreterRunner dynamic_interpreter = 0;

  void** VMMethod::instructions = 0;

  void VMMethod::init(STATE) {
    // Seed the instructions table
    Arguments args;
    interpreter(0, 0, 0, args);

#ifdef USE_DYNAMIC_INTERPRETER
    if(!state->shared.config.dynamic_interpreter_enabled) {
      dynamic_interpreter = NULL;
      standard_interpreter = &VMMethod::interpreter;
      return;
    }

    /*
    if(dynamic_interpreter == NULL) {
      uint8_t* buffer = new uint8_t[1024 * 1024 * 1024];
      JITCompiler jit(buffer);
      jit.create_interpreter(state);
      if(getenv("DUMP_DYN")) {
        jit.assembler().show();
      }

      dynamic_interpreter = reinterpret_cast<Runner>(buffer);
    }
    */

    standard_interpreter = dynamic_interpreter;
#else
    dynamic_interpreter = NULL;
    standard_interpreter = &VMMethod::interpreter;
#endif
  }

  /*
   * Turns a CompiledMethod's InstructionSequence into a C array of opcodes.
   */
  VMMethod::VMMethod(STATE, CompiledMethod* meth)
    : parent_(NULL)
    , run(standard_interpreter)
    , original(state, meth)
    , type(NULL)
    , native_function(NULL)
    , jitted_(false)
#ifdef ENABLE_LLVM
    , llvm_function_(NULL)
#endif
  {
    meth->set_executor(&VMMethod::execute);

    total = meth->iseq()->opcodes()->num_fields();
    if(Tuple* tup = try_as<Tuple>(meth->literals())) {
      blocks.resize(tup->num_fields(), NULL);
    }

    opcodes = new opcode[total];
    addresses = new void*[total];

    fill_opcodes(state);
    stack_size =    meth->stack_size()->to_native();
    number_of_locals = meth->number_of_locals();

    total_args =    meth->total_args()->to_native();
    required_args = meth->required_args()->to_native();
    if(meth->splat()->nil_p()) {
      splat_position = -1;
    } else {
      splat_position = as<Integer>(meth->splat())->to_native();
    }

    setup_argument_handler(meth);

    // Disable JIT for large methods
    if(meth->primitive()->nil_p() &&
        !state->shared.config.jit_disabled &&
        total < (size_t)state->shared.config.jit_max_method_size) {
      call_count = 0;
    } else {
      call_count = -1;
    }
  }

  VMMethod::~VMMethod() {
    delete[] opcodes;
  }

  void VMMethod::fill_opcodes(STATE) {
    Tuple* ops = original->iseq()->opcodes();
    Object* val;

    int sends = 0;

    for(size_t index = 0; index < total;) {
      val = ops->at(state, index);
      if(val->nil_p()) {
        opcodes[index++] = 0;
      } else {
        opcodes[index] = as<Fixnum>(val)->to_native();

        size_t width = InstructionSequence::instruction_width(opcodes[index]);

        switch(width) {
        case 2:
          opcodes[index + 1] = as<Fixnum>(ops->at(state, index + 1))->to_native();
          break;
        case 3:
          opcodes[index + 1] = as<Fixnum>(ops->at(state, index + 1))->to_native();
          opcodes[index + 2] = as<Fixnum>(ops->at(state, index + 2))->to_native();
          break;
        }

        update_addresses(index, width - 1);

        switch(opcodes[index]) {
        case InstructionSequence::insn_send_method:
        case InstructionSequence::insn_send_stack:
        case InstructionSequence::insn_send_stack_with_block:
        case InstructionSequence::insn_send_stack_with_splat:
        case InstructionSequence::insn_send_super_stack_with_block:
        case InstructionSequence::insn_send_super_stack_with_splat:
        case InstructionSequence::insn_zsuper:
        case InstructionSequence::insn_meta_send_call:
        case InstructionSequence::insn_meta_send_op_plus:
        case InstructionSequence::insn_meta_send_op_minus:
        case InstructionSequence::insn_meta_send_op_equal:
        case InstructionSequence::insn_meta_send_op_tequal:
        case InstructionSequence::insn_meta_send_op_lt:
        case InstructionSequence::insn_meta_send_op_gt:
        case InstructionSequence::insn_check_serial:
        case InstructionSequence::insn_check_serial_private:
          sends++;
        }

        index += width;
      }
    }

    initialize_caches(state, sends);
  }

  void VMMethod::initialize_caches(STATE, int sends) {
    number_of_caches_ = sends;
    caches = new InlineCache[sends];

    int which = 0;
    bool allow_private = false;
    bool is_super = false;

    for(size_t ip = 0; ip < total;) {
      opcode op = opcodes[ip];
      switch(op) {
      case InstructionSequence::insn_allow_private:
        allow_private = true;
        break;
      case InstructionSequence::insn_send_super_stack_with_block:
      case InstructionSequence::insn_send_super_stack_with_splat:
      case InstructionSequence::insn_zsuper:
        is_super = true;
        // fall through
      case InstructionSequence::insn_check_serial:
      case InstructionSequence::insn_check_serial_private:
      case InstructionSequence::insn_send_method:
      case InstructionSequence::insn_send_stack:
      case InstructionSequence::insn_send_stack_with_block:
      case InstructionSequence::insn_send_stack_with_splat:
      case InstructionSequence::insn_meta_send_call:
      case InstructionSequence::insn_meta_send_op_plus:
      case InstructionSequence::insn_meta_send_op_minus:
      case InstructionSequence::insn_meta_send_op_equal:
      case InstructionSequence::insn_meta_send_op_tequal:
      case InstructionSequence::insn_meta_send_op_lt:
      case InstructionSequence::insn_meta_send_op_gt: {
        assert(which < sends);
        InlineCache* cache = &caches[which++];
        cache->set_location(ip, this);

        Symbol* name = try_as<Symbol>(original->literals()->at(opcodes[ip + 1]));
        if(!name) {
          name = state->symbol("__unknown__");
        }

        cache->set_name(name);

        if(allow_private) cache->set_is_private();
        if(is_super) cache->set_is_super();

        state->shared.ic_registry()->add_cache(name, cache);

        opcodes[ip + 1] = reinterpret_cast<intptr_t>(cache);
        update_addresses(ip, 1);

        is_super = false;
        allow_private = false;
      }
      }

      ip += InstructionSequence::instruction_width(op);
    }
  }

  // Argument handler implementations

#ifdef USE_SPECIALIZED_EXECUTE

  // For when the method expects no arguments at all (no splat, nothing)
  class NoArguments {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      return args.total() == 0;
    }
  };

  // For when the method expects 1 and only 1 argument
  class OneArgument {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      if(args.total() != 1) return false;
      scope->set_local(0, args.get_argument(0));
      return true;
    }
  };

  // For when the method expects 2 and only 2 arguments
  class TwoArguments {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      if(args.total() != 2) return false;
      scope->set_local(0, args.get_argument(0));
      scope->set_local(1, args.get_argument(1));
      return true;
    }
  };

  // For when the method expects 3 and only 3 arguments
  class ThreeArguments {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      if(args.total() != 3) return false;
      scope->set_local(0, args.get_argument(0));
      scope->set_local(1, args.get_argument(1));
      scope->set_local(2, args.get_argument(2));
      return true;
    }
  };

  // For when the method expects a fixed number of arguments (no splat)
  class FixedArguments {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      if((native_int)args.total() != vmm->total_args) return false;

      for(native_int i = 0; i < vmm->total_args; i++) {
        scope->set_local(i, args.get_argument(i));
      }

      return true;
    }
  };

  // For when a method takes all arguments as a splat
  class SplatOnlyArgument {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      const size_t total = args.total();
      Array* ary = Array::create(state, total);

      for(size_t i = 0; i < total; i++) {
        ary->set(state, i, args.get_argument(i));
      }

      scope->set_local(vmm->splat_position, ary);
      return true;
    }
  };
#endif

  // The fallback, can handle all cases
  class GenericArguments {
  public:
    bool call(STATE, VMMethod* vmm, StackVariables* scope, Arguments& args) {
      const bool has_splat = (vmm->splat_position >= 0);

      // expecting 0, got 0.
      if(vmm->total_args == 0 and args.total() == 0) {
        if(has_splat) {
          scope->set_local(vmm->splat_position, Array::create(state, 0));
        }

        return true;
      }

      // Too few args!
      if((native_int)args.total() < vmm->required_args) return false;

      // Too many args (no splat!)
      if(!has_splat && (native_int)args.total() > vmm->total_args) return false;

      // Umm... something too do with figuring out how to handle
      // splat and optionals.
      native_int fixed_args = vmm->total_args;
      if((native_int)args.total() < vmm->total_args) {
        fixed_args = (native_int)args.total();
      }

      // Copy in the normal, fixed position arguments
      for(native_int i = 0; i < fixed_args; i++) {
        scope->set_local(i, args.get_argument(i));
      }

      if(has_splat) {
        Array* ary;
        /* There is a splat. So if the passed in arguments are greater
         * than the total number of fixed arguments, put the rest of the
         * arguments into the Array.
         *
         * Otherwise, generate an empty Array.
         *
         * NOTE: remember that total includes the number of fixed arguments,
         * even if they're optional, so we can get args.total() == 0, and
         * total == 1 */
        if((native_int)args.total() > vmm->total_args) {
          size_t splat_size = args.total() - vmm->total_args;
          ary = Array::create(state, splat_size);

          for(size_t i = 0, n = vmm->total_args; i < splat_size; i++, n++) {
            ary->set(state, i, args.get_argument(n));
          }
        } else {
          ary = Array::create(state, 0);
        }

        scope->set_local(vmm->splat_position, ary);
      }

      return true;
    }
  };

  /*
   * Looks at the opcodes for this method and optimizes instance variable
   * access by using special byte codes.
   *
   * For push_ivar, uses push_my_field when the instance variable has an
   * index assigned.  Same for set_ivar/store_my_field.
   */

  void VMMethod::specialize(STATE, TypeInfo* ti) {
    type = ti;
    for(size_t i = 0; i < total;) {
      opcode op = opcodes[i];

      if(op == InstructionSequence::insn_push_ivar) {
        native_int idx = opcodes[i + 1];
        native_int sym = as<Symbol>(original->literals()->at(state, idx))->index();

        TypeInfo::Slots::iterator it = ti->slots.find(sym);
        if(it != ti->slots.end()) {
          opcodes[i] = InstructionSequence::insn_push_my_offset;
          opcodes[i + 1] = ti->slot_locations[it->second];

          update_addresses(i, 1);
        }
      } else if(op == InstructionSequence::insn_set_ivar) {
        native_int idx = opcodes[i + 1];
        native_int sym = as<Symbol>(original->literals()->at(state, idx))->index();

        TypeInfo::Slots::iterator it = ti->slots.find(sym);
        if(it != ti->slots.end()) {
          opcodes[i] = InstructionSequence::insn_store_my_field;
          opcodes[i + 1] = it->second;

          update_addresses(i, 1);
        }
      }

      i += InstructionSequence::instruction_width(op);
    }

    find_super_instructions();
  }

  void VMMethod::find_super_instructions() {
    return;
    for(size_t index = 0; index < total;) {
      size_t width = InstructionSequence::instruction_width(opcodes[index]);
      int super = instructions::find_superop(&opcodes[index]);
      if(super > 0) {
        opcodes[index] = (opcode)super;
      }
      index += width;
    }
  }

  void VMMethod::setup_argument_handler(CompiledMethod* meth) {
#ifdef USE_SPECIALIZED_EXECUTE
    // If there are no optionals, only a fixed number of positional arguments.
    if(total_args == required_args) {
      // if no arguments are expected
      if(total_args == 0) {
        // and there is no splat, use the fastest case.
        if(splat_position == -1) {
          meth->set_executor(&VMMethod::execute_specialized<NoArguments>);

        // otherwise use the splat only case.
        } else {
          meth->set_executor(&VMMethod::execute_specialized<SplatOnlyArgument>);
        }
        return;

      // Otherwise use the few specialized cases iff there is no splat
      } else if(splat_position == -1) {
        switch(total_args) {
        case 1:
          meth->set_executor(&VMMethod::execute_specialized<OneArgument>);
          return;
        case 2:
          meth->set_executor(&VMMethod::execute_specialized<TwoArguments>);
          return;
        case 3:
          meth->set_executor(&VMMethod::execute_specialized<ThreeArguments>);
          return;
        default:
          meth->set_executor(&VMMethod::execute_specialized<FixedArguments>);
          return;
        }
      }
    }

    // Lastly, use the generic case that handles all cases
    meth->set_executor(&VMMethod::execute_specialized<GenericArguments>);
#endif
  }

  /* This is the execute implementation used by normal Ruby code,
   * as opposed to Primitives or FFI functions.
   * It prepares a Ruby method for execution.
   * Here, +exec+ is a VMMethod instance accessed via the +vmm+ slot on
   * CompiledMethod.
   *
   * This method works as a template even though it's here because it's never
   * called from outside of this file. Thus all the template expansions are done.
   */
  template <typename ArgumentHandler>
    Object* VMMethod::execute_specialized(STATE, CallFrame* previous,
        Dispatch& msg, Arguments& args) {

      CompiledMethod* cm = as<CompiledMethod>(msg.method);
      VMMethod* vmm = cm->backend_method();

#ifdef ENABLE_LLVM
      // A negative call_count means we've disabled usage based JIT
      // for this method.
      if(vmm->call_count >= 0) {
        if(vmm->call_count >= state->shared.config.jit_call_til_compile) {
          LLVMState* ls = LLVMState::get(state);
          ls->compile_callframe(state, vmm, previous);
        } else {
          vmm->call_count++;
        }
      }
#endif

      size_t scope_size = sizeof(StackVariables) +
        (vmm->number_of_locals * sizeof(Object*));
      StackVariables* scope =
        reinterpret_cast<StackVariables*>(alloca(scope_size));
      // Originally, I tried using msg.module directly, but what happens is if
      // super is used, that field is read. If you combine that with the method
      // being called recursively, msg.module can change, causing super() to
      // look in the wrong place.
      //
      // Thus, we have to cache the value in the StackVariables.
      scope->initialize(args.recv(), args.block(), msg.module, vmm->number_of_locals);

      InterpreterCallFrame* frame = ALLOCA_CALLFRAME(vmm->stack_size);

      // If argument handling fails..
      ArgumentHandler arghandler;
      if(arghandler.call(state, vmm, scope, args) == false) {
        Exception* exc =
          Exception::make_argument_error(state, vmm->required_args, args.total(), msg.name);
        exc->locations(state, System::vm_backtrace(state, Fixnum::from(0), previous));
        state->thread_state()->raise_exception(exc);

        return NULL;
      }

      frame->prepare(vmm->stack_size);

      frame->previous = previous;
      frame->flags =    0;
      frame->msg =      &msg;
      frame->cm =       cm;
      frame->scope =    scope;


#ifdef RBX_PROFILER
      if(unlikely(state->shared.profiling())) {
        profiler::MethodEntry method(state, msg, args, cm);
        return (*vmm->run)(state, vmm, frame, args);
      } else {
        return (*vmm->run)(state, vmm, frame, args);
      }
#else
      return (*vmm->run)(state, vmm, frame, args);
#endif
    }

  /** This is used as a fallback way of entering the interpreter */
  Object* VMMethod::execute(STATE, CallFrame* previous, Dispatch& msg, Arguments& args) {
    return execute_specialized<GenericArguments>(state, previous, msg, args);
  }

  /* This is a noop for this class. */
  void VMMethod::compile(STATE) { }

  void VMMethod::deoptimize(STATE) {
#ifdef ENABLE_LLVM
    // This resets execute to use the interpreter
    setup_argument_handler(original.get());

    if(llvm_function_) {
      LLVMState::get(state)->remove(llvm_function_);
    }
    llvm_function_ = 0;

    // Don't reset call_count if it's -1
    // (why would it be -1, it shouldn't be if we got this far)
    if(call_count >= 0) call_count = 0;
#endif
  }

  /*
   * Turns a VMMethod into a C++ vector of Opcodes.
   */
  std::vector<Opcode*> VMMethod::create_opcodes() {
    std::vector<Opcode*> ops;
    std::map<int, size_t> stream2opcode;

    VMMethod::Iterator iter(this);

    /* Fill +ops+ with all our Opcode objects, maintain
     * the map from stream position to instruction. */
    for(size_t ipos = 0; !iter.end(); ipos++, iter.inc()) {
      stream2opcode[iter.position] = ipos;
      Opcode* lop = new Opcode(iter);
      ops.push_back(lop);
    }

    /* Iterate through the ops, fixing goto locations to point
     * to opcodes and set start_block on any opcode that is
     * the beginning of a block */
    bool next_new = false;

    for(std::vector<Opcode*>::iterator i = ops.begin(); i != ops.end(); i++) {
      Opcode* op = *i;
      if(next_new) {
        op->start_block = true;
        next_new = false;
      }

      /* We patch and mark where we branch to. */
      if(op->is_goto()) {
        op->arg1 = stream2opcode[op->arg1];
        ops.at(op->arg1)->start_block = true;
      }

      /* This terminates the block. */
      if(op->is_terminator()) {
        /* this ends a block. */
        next_new = true;
      }
    }

    // TODO take the exception table into account here

    /* Go through again and assign each opcode a block
     * number. */
    size_t block = 0;
    for(std::vector<Opcode*>::iterator i = ops.begin(); i != ops.end(); i++) {
      Opcode* op = *i;

      if(op->start_block) block++;
      op->block = block;
    }

    return ops;
  }

  /*
   * Ensures the specified IP value is a valid address.
   */
  bool VMMethod::validate_ip(STATE, size_t ip) {
    /* Ensure ip is valid */
    VMMethod::Iterator iter(this);
    for(; !iter.end(); iter.inc()) {
      if(iter.position >= ip) break;
    }
    return ip == iter.position;
  }

  /*
   * Sets breakpoint flags on the specified opcode.
   */
  void VMMethod::set_breakpoint_flags(STATE, size_t ip, bpflags flags) {
    if(validate_ip(state, ip)) {
      opcodes[ip] &= cBreakpointMask;    // Clear the high byte
      opcodes[ip] |= flags & ~cBreakpointMask;
    }
  }

  /*
   * Gets breakpoint flags on the specified opcode.
   */
  bpflags VMMethod::get_breakpoint_flags(STATE, size_t ip) {
    if(validate_ip(state, ip)) {
      return opcodes[ip] & ~cBreakpointMask;
    }
    return 0;
  }

  Object** VMMethod::add_indirect_literal(Object* obj) {
    Object** ptr = new Object*[1];
    *ptr = obj;
    indirect_literals_.push_back(ptr);
    return ptr;
  }


  bool Opcode::is_goto() {
    switch(op) {
    case InstructionSequence::insn_goto_if_false:
    case InstructionSequence::insn_goto_if_true:
    case InstructionSequence::insn_goto:
      return true;
    }

    return false;
  }

  bool Opcode::is_terminator() {
    switch(op) {
    case InstructionSequence::insn_send_method:
    case InstructionSequence::insn_send_stack:
    case InstructionSequence::insn_send_stack_with_block:
    case InstructionSequence::insn_send_stack_with_splat:
    case InstructionSequence::insn_meta_send_op_plus:
    case InstructionSequence::insn_meta_send_op_minus:
    case InstructionSequence::insn_meta_send_op_equal:
    case InstructionSequence::insn_meta_send_op_lt:
    case InstructionSequence::insn_meta_send_op_gt:
    case InstructionSequence::insn_meta_send_op_tequal:
      return true;
    }

    return false;
  }

  bool Opcode::is_send() {
    switch(op) {
    case InstructionSequence::insn_send_method:
    case InstructionSequence::insn_send_stack:
    case InstructionSequence::insn_send_stack_with_block:
    case InstructionSequence::insn_send_stack_with_splat:
    case InstructionSequence::insn_meta_send_op_plus:
    case InstructionSequence::insn_meta_send_op_minus:
    case InstructionSequence::insn_meta_send_op_equal:
    case InstructionSequence::insn_meta_send_op_lt:
    case InstructionSequence::insn_meta_send_op_gt:
    case InstructionSequence::insn_meta_send_op_tequal:
      return true;
    }

    return false;
  }
}
