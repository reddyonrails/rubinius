#include <cstdlib>
#include <iostream>

#include "vm.hpp"
#include "objectmemory.hpp"
#include "gc/marksweep.hpp"
#include "gc/baker.hpp"
#include "gc/immix.hpp"
#include "gc/inflated_headers.hpp"

#include "config_parser.hpp"

#include "builtin/class.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/tuple.hpp"

#include "capi/handle.hpp"
#include "configuration.hpp"

#include "global_cache.hpp"

namespace rubinius {

  Object* object_watch = 0;

  /* ObjectMemory methods */
  ObjectMemory::ObjectMemory(STATE, Configuration& config)
    : young_(new BakerGC(this, config.gc_bytes))
    , mark_sweep_(new MarkSweepGC(this))
    , immix_(new ImmixGC(this))
    , inflated_headers_(new InflatedHeaders)
    , mark_(1)
    , state(state)
  {
    // TODO Not sure where this code should be...
    if(char* num = getenv("RBX_WATCH")) {
      object_watch = (Object*)strtol(num, NULL, 10);
      std::cout << "Watching for " << object_watch << "\n";
    }

    remember_set_ = new ObjectArray(0);

    collect_mature_now = false;
    last_object_id = 0;

    large_object_threshold = config.gc_large_object;
    young_->set_lifetime(config.gc_lifetime);

    if(config.gc_autotune) young_->set_autotune();

    for(size_t i = 0; i < LastObjectType; i++) {
      type_info[i] = NULL;
    }

    TypeInfo::init(this);
  }

  ObjectMemory::~ObjectMemory() {

    mark_sweep_->free_objects();

    // TODO free immix data

    delete remember_set_;

    for(size_t i = 0; i < LastObjectType; i++) {
      if(type_info[i]) delete type_info[i];
    }

    delete immix_;
    delete mark_sweep_;
    delete young_;
  }

  ObjectArray* ObjectMemory::swap_remember_set() {
    ObjectArray* cur = remember_set_;
    remember_set_ = new ObjectArray(0);
    return cur;
  }

  Object* ObjectMemory::new_object_fast(Class* cls, size_t bytes, object_type type) {
    if(Object* obj = young_->raw_allocate(bytes, &collect_young_now)) {
      if(collect_young_now) state->interrupts.set_perform_gc();
      obj->init_header(cls, YoungObjectZone, type);
      obj->clear_fields(bytes);
      return obj;
    } else {
      return new_object_typed(cls, bytes, type);
    }
  }


  void ObjectMemory::set_young_lifetime(size_t age) {
    young_->set_lifetime(age);
  }

  void ObjectMemory::debug_marksweep(bool val) {
    if(val) {
      mark_sweep_->free_entries = false;
    } else {
      mark_sweep_->free_entries = true;
    }
  }

  bool ObjectMemory::valid_object_p(Object* obj) {
    if(obj->young_object_p()) {
      return young_->validate_object(obj) == cValid;
    } else if(obj->mature_object_p()) {
      return true;
    } else {
      return false;
    }
  }

  /* Garbage collection */

  Object* ObjectMemory::promote_object(Object* obj) {
#ifdef RBX_GC_STATS
    stats::GCStats::get()->objects_promoted++;
#endif

    Object* copy = immix_->allocate(obj->size_in_bytes(state));

    copy->set_obj_type(obj->type_id());
    copy->initialize_copy(obj, 0);
    copy->copy_body(state, obj);

    if(watched_p(obj)) {
      std::cout << "detected object " << obj << " during promotion.\n";
    }

    return copy;
  }

  void ObjectMemory::collect_young(GCData& data, YoungCollectStats* stats) {
    collect_young_now = false;

    static int collect_times = 0;

    // validate_handles(data.handles());
    // validate_handles(data.cached_handles());

    young_->collect(data, stats);
    prune_handles(data.handles(), true);
    prune_handles(data.cached_handles(), true);
    collect_times++;

    data.global_cache()->prune_young();
  }

  void ObjectMemory::collect_mature(GCData& data) {
#ifdef RBX_GC_STATS
    stats::GCStats::get()->objects_seen.start();
    stats::GCStats::get()->collect_mature.start();
#endif

    // validate_handles(data.handles());
    // validate_handles(data.cached_handles());

    collect_mature_now = false;
    immix_->collect(data);

    data.global_cache()->prune_unmarked(mark());

    immix_->clean_weakrefs();
    prune_handles(data.handles(), false);
    prune_handles(data.cached_handles(), false);

    // Have to do this after all things that check for mark bits is
    // done, as it free()s objects, invalidating mark bits.
    mark_sweep_->after_marked();

    inflated_headers_->deallocate_headers(mark());

    // We no longer need to unmark all, we use the rotating mark instead.
    // This means that the mark we just set on all reachable objects will
    // be ignored next time anyway.
    //
    // immix_->unmark_all(data);

    rotate_mark();

#ifdef RBX_GC_STATS
    stats::GCStats::get()->collect_mature.stop();
    stats::GCStats::get()->objects_seen.stop();
#endif
  }

  InflatedHeader* ObjectMemory::inflate_header(ObjectHeader* obj) {
    if(obj->inflated_header_p()) return obj->inflated_header();

    InflatedHeader* header = inflated_headers_->allocate(obj);
    obj->set_inflated_header(header);
    return header;
  }

  void ObjectMemory::validate_handles(capi::Handles* handles) {
    capi::Handle* handle = handles->front();
    capi::Handle* current;

    while(handle) {
      current = handle;
      handle = static_cast<capi::Handle*>(handle->next());

      Object* obj = current->object();

      assert(obj->inflated_header_p());
      InflatedHeader* ih = obj->inflated_header();

      assert(ih->handle() == current);
      assert(ih->object() == obj);
    }
  }

  void ObjectMemory::prune_handles(capi::Handles* handles, bool check_forwards) {
    capi::Handle* handle = handles->front();
    capi::Handle* current;

    int total = 0;
    int count = 0;

    while(handle) {
      current = handle;
      handle = static_cast<capi::Handle*>(handle->next());

      Object* obj = current->object();
      total++;

      // Strong references will already have been updated.
      if(!current->weak_p()) {
        if(check_forwards) assert(!obj->forwarded_p());
        assert(obj->inflated_header()->object() == obj);
      } else if(check_forwards) {
        if(obj->young_object_p()) {

          // A weakref pointing to a valid young object
          //
          // TODO this only works because we run prune_handles right after
          // a collection. In this state, valid objects are only in current.
          if(young_->in_current_p(obj)) {
            continue;

          // A weakref pointing to a forwarded young object
          } else if(obj->forwarded_p()) {
            current->set_object(obj->forward());
            assert(current->object()->inflated_header_p());
            assert(current->object()->inflated_header()->object() == current->object());

          // A weakref pointing to a dead young object
          } else {
            count++;
            handles->remove(current);
            delete current;
          }
        }

      // A weakref pointing to a dead mature object
      } else if(!obj->marked_p(mark())) {
        count++;
        handles->remove(current);
        delete current;
      } else {
        assert(obj->inflated_header()->object() == obj);
      }
    }

    // std::cout << "Pruned " << count << " handles, " << total << "/" << handles->size() << " total.\n";
  }

  int ObjectMemory::mature_bytes_allocated() {
    return immix_->bytes_allocated() + mark_sweep_->allocated_bytes;
  }

  void ObjectMemory::add_type_info(TypeInfo* ti) {
    type_info[ti->type] = ti;
  }

  /* Store an object into the remember set. Called when we've calculated
   * externally that the object in question needs to be remembered */
  void ObjectMemory::remember_object(Object* target) {
    assert(target->zone() == MatureObjectZone);
    /* If it's already remembered, ignore this request */
    if(target->remembered_p()) return;
    target->set_remember();
    remember_set_->push_back(target);
  }

  void ObjectMemory::unremember_object(Object* target) {
    for(ObjectArray::iterator oi = remember_set_->begin();
        oi != remember_set_->end();
        oi++) {
      if(*oi == target) {
        *oi = NULL;
        target->clear_remember();
      }
    }
  }

  Object* ObjectMemory::allocate_object(size_t bytes) {
    Object* obj;

    if(unlikely(bytes > large_object_threshold)) {
      obj = mark_sweep_->allocate(bytes, &collect_mature_now);
      if(collect_mature_now) {
        state->interrupts.set_perform_gc();
      }

#ifdef RBX_GC_STATS
    stats::GCStats::get()->large_objects++;
#endif

    } else {
      obj = young_->allocate(bytes);
      if(unlikely(obj == NULL)) {
        collect_young_now = true;
        state->interrupts.set_perform_gc();

        obj = immix_->allocate(bytes);
        if(collect_mature_now) {
          state->interrupts.set_perform_gc();
        }
      }
    }

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during allocation\n";
    }
#endif

    obj->clear_fields(bytes);
    return obj;
  }

  Object* ObjectMemory::allocate_object_mature(size_t bytes) {
    Object* obj;

    if(bytes > large_object_threshold) {
      obj = mark_sweep_->allocate(bytes, &collect_mature_now);
      if(collect_mature_now) {
        state->interrupts.set_perform_gc();
      }

#ifdef RBX_GC_STATS
    stats::GCStats::get()->large_objects++;
#endif

    } else {
      obj = immix_->allocate(bytes);
      if(collect_mature_now) {
        state->interrupts.set_perform_gc();
      }
    }

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during mature allocation\n";
    }
#endif

    obj->clear_fields(bytes);
    return obj;
  }

  Object* ObjectMemory::new_object_typed(Class* cls, size_t bytes, object_type type) {
    Object* obj;

#ifdef RBX_GC_STATS
    stats::GCStats::get()->young_object_types[type]++;
#endif

    obj = allocate_object(bytes);
    obj->klass(this, cls);

    obj->set_obj_type(type);
    obj->set_requires_cleanup(type_info[type]->instances_need_cleanup);

    return obj;
  }

  Object* ObjectMemory::new_object_typed_mature(Class* cls, size_t bytes, object_type type) {
    Object* obj;

#ifdef RBX_GC_STATS
    stats::GCStats::get()->mature_object_types[type]++;
#endif

    obj = allocate_object_mature(bytes);
    obj->klass(this, cls);

    obj->set_obj_type(type);
    obj->set_requires_cleanup(type_info[type]->instances_need_cleanup);

    return obj;
  }

  /* ONLY use to create Class, the first object. */
  Object* ObjectMemory::allocate_object_raw(size_t bytes) {
    Object* obj = mark_sweep_->allocate(bytes, &collect_mature_now);
    obj->clear_fields(bytes);
    return obj;
  }

  Object* ObjectMemory::new_object_typed_enduring(Class* cls, size_t bytes, object_type type) {
#ifdef RBX_GC_STATS
    stats::GCStats::get()->mature_object_types[type]++;
#endif

    Object* obj = mark_sweep_->allocate(bytes, &collect_mature_now);
    if(collect_mature_now) {
      state->interrupts.set_perform_gc();
    }

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during enduring allocation\n";
    }
#endif

    obj->clear_fields(bytes);

#ifdef RBX_GC_STATS
    stats::GCStats::get()->large_objects++;
#endif

    obj->klass(this, cls);

    obj->set_obj_type(type);
    obj->set_requires_cleanup(type_info[type]->instances_need_cleanup);

    return obj;
  }

  TypeInfo* ObjectMemory::find_type_info(Object* obj) {
    return type_info[obj->type_id()];
  }

  ObjectPosition ObjectMemory::validate_object(Object* obj) {
    ObjectPosition pos;

    pos = young_->validate_object(obj);
    if(pos != cUnknown) return pos;

    pos = immix_->validate_object(obj);
    if(pos != cUnknown) return pos;

    return mark_sweep_->validate_object(obj);
  }

  bool ObjectMemory::valid_young_object_p(Object* obj) {
    return obj->young_object_p() && young_->in_current_p(obj);
  }
};

#define DEFAULT_MALLOC_THRESHOLD 10000000

static long bytes_until_collection = DEFAULT_MALLOC_THRESHOLD;

void* XMALLOC(size_t bytes) {
  bytes_until_collection -= bytes;
  if(bytes_until_collection <= 0) {
    rubinius::VM::current_state()->run_gc_soon();
    bytes_until_collection = DEFAULT_MALLOC_THRESHOLD;
  }
  return malloc(bytes);
}

void XFREE(void* ptr) {
  free(ptr);
}

void* XREALLOC(void* ptr, size_t bytes) {
  bytes_until_collection -= bytes;
  if(bytes_until_collection <= 0) {
    rubinius::VM::current_state()->run_gc_soon();
    bytes_until_collection = DEFAULT_MALLOC_THRESHOLD;
  }

  return realloc(ptr, bytes);
}

void* XCALLOC(size_t items, size_t bytes_per) {
  size_t bytes = bytes_per * items;

  bytes_until_collection -= bytes;
  if(bytes_until_collection <= 0) {
    rubinius::VM::current_state()->run_gc_soon();
    bytes_until_collection = DEFAULT_MALLOC_THRESHOLD;
  }

  return calloc(items, bytes_per);
}

