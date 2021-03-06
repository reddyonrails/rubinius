#include "vm.hpp"
#include "vm/object_utils.hpp"

#include "builtin/class.hpp"
#include "builtin/executable.hpp"
#include "builtin/symbol.hpp"

#include "arguments.hpp"
#include "dispatch.hpp"
#include "call_frame.hpp"
#include "objectmemory.hpp"

namespace rubinius {

  void Executable::init(STATE) {
    GO(executable).set(state->new_class("Executable", G(object), G(rubinius)));
    G(executable)->set_object_type(state, ExecutableType);
    G(executable)->name(state, state->symbol("Rubinius::Executable"));
  }

  Executable* Executable::allocate(STATE, Object* self) {
    Executable* executable = state->new_object<Executable>(G(executable));
    executable->primitive(state, (Symbol*)Qnil);
    executable->serial(state, Fixnum::from(0));
    executable->inliners_ = 0;
    executable->prim_index_ = -1;

    executable->set_executor(Executable::default_executor);

    if(Class* cls = try_as<Class>(self)) {
      executable->klass(state, cls);
    }

    return executable;
  }

  bool Executable::resolve_primitive(STATE) {
    if(!primitive_->nil_p()) {
      if(Symbol* name = try_as<Symbol>(primitive_)) {
        set_executor(Primitives::resolve_primitive(state, name, &prim_index_));
        return true;
      }
    }

    return false;
  }

  Object* Executable::invoke(STATE, Symbol* name, Module* mod, Object* recv, Array* ary,
                             Object* block, CallFrame* call_frame)
  {
    Dispatch disp(name, mod, this);
    Arguments args(recv, 0, 0);
    args.use_array(ary);
    args.set_block(block);

    return execute(state, call_frame, disp, args);
  }

  Object* Executable::default_executor(STATE, CallFrame* call_frame, Dispatch& msg,
                                       Arguments& args) {
    args.unshift2(state, args.recv(), msg.name);
    args.set_recv(msg.method);

    Dispatch dis(state->symbol("call"));
    return dis.send(state, call_frame, args);
  }

  void Executable::add_inliner(VMMethod* vmm) {
    if(!inliners_ || inliners_ == (Inliners*)Qnil) inliners_ = new Inliners;
    inliners_->push_back(vmm);
  }

  void Executable::clear_inliners(STATE) {
    if(!inliners_ || inliners_ == (Inliners*)Qnil) return;
    for(Inliners::const_iterator i = inliners_->begin();
        i != inliners_->end();
        i++) {
      (*i)->deoptimize(state);
    }

    inliners_->clear();
  }
}
