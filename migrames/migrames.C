#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdbool.h>
#include "flatbuffers/flatbuffers.h"
#include "py_object_generated.h"
#include "utils.h"
#include "serdes.h"
#include "pyref.h" 
#include "py_structs.h"


class migrames_modulestate {
    public:
        pyobject_strongref deepcopy;
        pyobject_strongref deepcopy_module;
        pyobject_strongref pickle_module;
        pyobject_strongref pickle_dumps;
        pyobject_strongref pickle_loads;
        migrames_modulestate() {
            deepcopy_module = PyImport_ImportModule("copy");
            deepcopy = PyObject_GetAttrString(*deepcopy_module, "deepcopy");
            pickle_module = PyImport_ImportModule("pickle");
            pickle_dumps = PyObject_GetAttrString(*pickle_module, "dumps");
            pickle_loads = PyObject_GetAttrString(*pickle_module, "loads");
        }

};

class dumps_functor {
    pyobject_weakref pickle_dumps;
    public:
    dumps_functor(pyobject_weakref pickle_dumps) : pickle_dumps(pickle_dumps) {}

    pyobject_strongref operator()(PyObject *obj) {
        PyObject *result = PyObject_CallOneArg(*pickle_dumps, obj);
        return pyobject_strongref::steal(result);
    }
};

class loads_functor {
    pyobject_weakref pickle_loads;
    public:
    loads_functor(pyobject_weakref pickle_loads) : pickle_loads(pickle_loads) {}

    pyobject_strongref operator()(PyObject *obj) {
        PyObject *result = PyObject_CallOneArg(*pickle_loads, obj);
        return pyobject_strongref::steal(result);
    }
};

static migrames_modulestate *migrames_state;

extern "C" {


static inline _PyStackRef *_PyFrame_Stackbase(_PyInterpreterFrame *f) {
    return f->localsplus + ((PyCodeObject*)f->f_executable.bits)->co_nlocalsplus;
}

bool ThreadState_HasStackSpace(PyThreadState *state, int size) {
    return state->datastack_top != NULL && size < state->datastack_limit - state->datastack_top;
}
_PyInterpreterFrame *ThreadState_PushFrame(PyThreadState *tstate, size_t size) {
    if(!ThreadState_HasStackSpace(tstate, size)) {
        return NULL;
    }
    _PyInterpreterFrame *top = (_PyInterpreterFrame*) tstate->datastack_top;
    tstate->datastack_top += size;
    return top;
}

void copy_stack(_PyInterpreterFrame *src, _PyInterpreterFrame *dest) {
    if (src->stackpointer == NULL) {
        return;
    }
    _PyStackRef *src_stack_base = _PyFrame_Stackbase(src);
    _PyStackRef *dest_stack_base = _PyFrame_Stackbase(dest);
    Py_ssize_t stack_size = src->stackpointer - src_stack_base;
    memcpy(dest_stack_base, src_stack_base, stack_size * sizeof(_PyStackRef));
    dest->stackpointer = dest_stack_base + stack_size;
}



PyAPI_FUNC(PyFrameObject *) PyFrame_New(PyThreadState *, PyCodeObject *,
                                        PyObject *, PyObject *);

typedef struct serialized_obj {
    char *data;
    size_t size;
} serialized_obj;

PyObject *GetFrameLocalsFromFrame(PyObject *frame) {
    PyFrameObject *current_frame = (PyFrameObject *)frame;

    PyObject *locals = PyFrame_GetLocals(current_frame);
    if (locals == NULL) {
        return NULL;
    }


    if (PyFrameLocalsProxy_Check(locals)) {
        PyObject* ret = PyDict_New();
        if (ret == NULL) {
            Py_DECREF(locals);
            return NULL;
        }
        if (PyDict_Update(ret, locals) < 0) {
            Py_DECREF(ret);
            Py_DECREF(locals);
            return NULL;
        }
        Py_DECREF(locals);


        return ret;
    }

    assert(PyMapping_Check(locals));
    return locals;
}


// Allocate something that's not part of Python
_PyInterpreterFrame *AllocateFrameToMigrate(size_t size) {
    return (_PyInterpreterFrame *)malloc(size * sizeof(PyObject*));
}

PyObject *deepcopy_object(PyObject *obj) {
    if (obj == NULL) {
        return NULL;
    }
    PyObject *deepcopy = *migrames_state->deepcopy;
    PyObject *copy_obj = PyObject_CallFunction(deepcopy, "O", obj);
    return copy_obj;
}

struct frame_copy_capsule {
    // Strong reference
    PyFrameObject *frame;
    size_t offset;
};

static char copy_frame_capsule_name[] = "Frame Capsule Object";

void frame_copy_capsule_destroy(PyObject *capsule) {
    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)PyCapsule_GetPointer(capsule, copy_frame_capsule_name);
    Py_DECREF(copy_capsule->frame);
    free(copy_capsule);
}

PyObject *frame_copy_capsule_create(PyFrameObject *frame, size_t offset) {
    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)malloc(sizeof(struct frame_copy_capsule));
    copy_capsule->frame = frame;
    copy_capsule->offset = offset;
    return PyCapsule_New(copy_capsule, copy_frame_capsule_name, frame_copy_capsule_destroy);
}

void copy_localsplus(_PyInterpreterFrame *to_copy, _PyInterpreterFrame *new_frame, int nlocals, int deepcopy) {
    if (deepcopy) {
        for (int i = 0; i < nlocals; i++) {
            PyObject *local = (PyObject*) to_copy->localsplus[i].bits;
            PyObject *local_copy = deepcopy_object(local);
            new_frame->localsplus[i].bits = (uintptr_t)local_copy;
        }
    } else {
        memcpy(new_frame->localsplus, to_copy->localsplus, nlocals * sizeof(_PyStackRef));
    }
}

PyFrameObject *create_copied_frame(PyThreadState *tstate, _PyInterpreterFrame *to_copy, PyCodeObject *copy_code_obj, PyObject *LocalCopy, size_t offset, int push_frame, int deepcopy_localsplus, int set_previous, int copy_stack_flag) {
    int nlocals = copy_code_obj->co_nlocalsplus;

    PyFrameObject *new_frame = PyFrame_New(tstate, copy_code_obj, to_copy->f_globals, LocalCopy);

    _PyInterpreterFrame *stack_frame;
    if (push_frame) {
        stack_frame = ThreadState_PushFrame(tstate, copy_code_obj->co_framesize);
    } else {
        stack_frame = AllocateFrameToMigrate(copy_code_obj->co_framesize);
    }

    if(stack_frame == NULL) {
        PySys_WriteStderr("<Migrames>: Could not allocate memory for new frame\n");
        return NULL;
    }

    new_frame->f_frame = stack_frame;

    copy_localsplus(to_copy, new_frame->f_frame, nlocals, deepcopy_localsplus);

    new_frame->f_frame->owner = to_copy->owner;
    new_frame->f_frame->previous = set_previous ? to_copy : NULL;
    new_frame->f_frame->f_funcobj = deepcopy_object(to_copy->f_funcobj);
    PyObject *executable_copy = deepcopy_object((PyObject*) to_copy->f_executable.bits);
    new_frame->f_frame->f_executable.bits = (uintptr_t)executable_copy;
    new_frame->f_frame->f_globals = to_copy->f_globals;
    new_frame->f_frame->f_builtins = to_copy->f_builtins;
    new_frame->f_frame->f_locals = to_copy->f_locals;
    new_frame->f_frame->frame_obj = new_frame;
    new_frame->f_frame->stackpointer = new_frame->f_frame->localsplus + nlocals;
    new_frame->f_frame->instr_ptr = (_CodeUnit*) (copy_code_obj->co_code_adaptive + offset);

    if (copy_stack_flag) {
        copy_stack(to_copy, new_frame->f_frame);
    }

    return new_frame;
}

PyFrameObject *push_frame_for_running(PyThreadState *tstate, _PyInterpreterFrame *to_push, PyCodeObject *code) {
    _PyInterpreterFrame *stack_frame = ThreadState_PushFrame(tstate, code->co_framesize);
    py_weakref<PyFrameObject> pyframe_object = to_push->frame_obj;
    if(stack_frame == NULL) {
        PySys_WriteStderr("<Migrames>: Could not allocate memory for new frame\n");
        return NULL;
    }

    copy_localsplus(to_push, stack_frame, code->co_nlocalsplus, 0);
    auto offset = utils::py::get_instr_offset(to_push->frame_obj);
    
    stack_frame->owner = to_push->owner;
    stack_frame->previous = PyEval_GetFrame()->f_frame;
    stack_frame->f_funcobj = to_push->f_funcobj;
    stack_frame->f_executable.bits = to_push->f_executable.bits;
    stack_frame->f_globals = to_push->f_globals;
    stack_frame->f_builtins = to_push->f_builtins;
    stack_frame->f_locals = to_push->f_locals;
    stack_frame->frame_obj = *pyframe_object;
    stack_frame->stackpointer = stack_frame->localsplus + code->co_nlocalsplus;
    stack_frame->instr_ptr = (_CodeUnit*) (code->co_code_adaptive + (offset-8));
    copy_stack(to_push, stack_frame);

    pyframe_object->f_frame = stack_frame;
    return *pyframe_object;
}

static PyObject *copy_frame(PyObject *self, PyObject *args) {
    using namespace utils;
    struct _frame *frame = (struct _frame*) PyEval_GetFrame();
    _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *)deepcopy_object((PyObject*)code);

    Py_ssize_t offset = py::get_instr_offset(frame) + py::get_offset_for_skipping_call();
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*)frame);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = create_copied_frame(tstate, to_copy, copy_code_obj, LocalCopy, offset, 0, 1, 0, 1);

    PyObject *capsule = frame_copy_capsule_create(new_frame, offset);
    Py_DECREF(copy_code_obj);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return capsule;
}

static PyObject *copy_and_run_frame(PyObject *self, PyObject *args) {
    using namespace utils;
    struct _frame *frame = (struct _frame*) PyEval_GetFrame();
    _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *)deepcopy_object((PyObject*)code);

    Py_ssize_t offset = py::get_instr_offset(frame) + py::get_offset_for_skipping_call();
    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*)frame);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = create_copied_frame(tstate, to_copy, copy_code_obj, LocalCopy, offset, 1, 0, 1, 1);

    PyObject *res = PyEval_EvalFrame(new_frame);
    Py_DECREF(copy_code_obj);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return res;
}

static PyObject *_copy_run_frame_from_capsule(PyObject *capsule) {
    if (PyErr_Occurred()) {
        PyErr_Print();
        return NULL;
    }

    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)PyCapsule_GetPointer(capsule, copy_frame_capsule_name);
    if (copy_capsule == NULL) {
        return NULL;
    }

    PyFrameObject *frame = copy_capsule->frame;
    size_t offset = copy_capsule->offset;
    _PyInterpreterFrame *to_copy = frame->f_frame;
    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    assert(code != NULL);
    PyCodeObject *copy_code_obj = (PyCodeObject *)deepcopy_object((PyObject*)code);

    PyObject *FrameLocals = GetFrameLocalsFromFrame((PyObject*)frame);
    PyObject *LocalCopy = PyDict_Copy(FrameLocals);

    PyFrameObject *new_frame = create_copied_frame(tstate, to_copy, copy_code_obj, LocalCopy, offset, 1, 0, 1, 0);

    PyObject *res = PyEval_EvalFrame(new_frame);
    Py_DECREF(copy_code_obj);
    Py_DECREF(LocalCopy);
    Py_DECREF(FrameLocals);

    return res;
}

static PyObject *run_frame(PyObject *self, PyObject *args) {
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return NULL;
    }
    return _copy_run_frame_from_capsule(capsule);
}

static PyObject* _serialize_frame_from_capsule(PyObject *capsule) {
    if (PyErr_Occurred()) {
        PyErr_Print();
        return NULL;
    }

    struct frame_copy_capsule *copy_capsule = (struct frame_copy_capsule *)PyCapsule_GetPointer(capsule, copy_frame_capsule_name);
    if (copy_capsule == NULL) {
        return NULL;
    }

    loads_functor loads(migrames_state->pickle_loads);
    dumps_functor dumps(migrames_state->pickle_dumps);

    flatbuffers::FlatBufferBuilder builder{1024};
    serdes::PyObjectSerdes po_serdes(loads, dumps);

    serdes::PyFrameSerdes frame_serdes{po_serdes};

    auto serialized_frame = frame_serdes.serialize(builder, *(static_cast<migrames::PyFrame*>(copy_capsule->frame)));
    builder.Finish(serialized_frame);
    auto buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    PyObject *bytes = PyBytes_FromStringAndSize((const char *)buf, size);
    return bytes;
}

static void init_code(PyCodeObject *obj, serdes::DeserializedCodeObject &code) {
    obj->co_consts = Py_NewRef(code.co_consts.borrow());
    obj->co_names = Py_NewRef(code.co_names.borrow());
    obj->co_exceptiontable = Py_NewRef(code.co_exceptiontable.borrow());

    obj->co_flags = code.co_flags;
    obj->co_argcount = code.co_argcount;
    obj->co_posonlyargcount = code.co_posonlyargcount;
    obj->co_kwonlyargcount = code.co_kwonlyargcount;
    obj->co_stacksize = code.co_stacksize;
    obj->co_firstlineno = code.co_firstlineno;

    obj->co_nlocalsplus = code.co_nlocalsplus;
    obj->co_framesize = code.co_framesize;
    obj->co_nlocals = code.co_nlocals;
    obj->co_ncellvars = code.co_ncellvars;
    obj->co_nfreevars = code.co_nfreevars;
    obj->co_version = code.co_version;

    obj->co_localsplusnames = Py_NewRef(code.co_localsplusnames.borrow());
    obj->co_localspluskinds = Py_NewRef(code.co_localspluskinds.borrow());

    obj->co_filename = Py_NewRef(code.co_filename.borrow());
    obj->co_name = Py_NewRef(code.co_name.borrow());
    obj->co_qualname = Py_NewRef(code.co_qualname.borrow());
    obj->co_linetable = Py_NewRef(code.co_linetable.borrow());

    memcpy(obj->co_code_adaptive, code.co_code_adaptive.data(), code.co_code_adaptive.size());
    migrames::PyBitcodeInstruction *bitcode = (migrames::PyBitcodeInstruction *) obj->co_code_adaptive;

    // initialize the rest of the fields
    obj->co_weakreflist = NULL;
    obj->co_executors = NULL;
    obj->_co_cached = NULL;
    obj->_co_instrumentation_version = 0;
    obj->_co_monitoring = NULL;
    obj->_co_firsttraceable = 0;
    obj->co_extra = NULL;
}

static PyCodeObject *create_pycode_object(serdes::DeserializedCodeObject& code_obj) {
    auto code_size = static_cast<Py_ssize_t>(code_obj.co_code_adaptive.size())/2;
    // NOTE: We're not handling the necessary here when
    // Py_GIL_DISABLED is defined.
    PyCodeObject *code = PyObject_NewVar(PyCodeObject, &PyCode_Type, code_size*2);
    init_code(code, code_obj);

    return code;
}

// TODO
static void init_frame(PyFrameObject *frame, py_weakref<PyCodeObject> code, serdes::DeserializedPyFrame& frame_obj) {
    frame->f_back = NULL;
    frame->f_frame = NULL;
    frame->f_trace = NULL;
    frame->f_extra_locals = NULL;
    frame->f_locals_cache = NULL;

    frame->f_lineno = frame_obj.f_lineno;
    frame->f_trace_lines = frame_obj.f_trace_lines;
    frame->f_trace_opcodes = frame_obj.f_trace_opcodes;
    
    if(NULL != *frame_obj.f_trace) {
        frame->f_trace = Py_NewRef(frame_obj.f_trace.borrow());
    }
    if(NULL != *frame_obj.f_extra_locals) {
        frame->f_extra_locals = Py_NewRef(frame_obj.f_extra_locals.borrow());
    }
    if(NULL != *frame_obj.f_locals_cache) {
        frame->f_locals_cache = Py_NewRef(frame_obj.f_locals_cache.borrow());
    }
}

static PyFrameObject *create_pyframe_object(serdes::DeserializedPyFrame& frame_obj, py_weakref<PyCodeObject> code) {
    // TODO: Double-check that this is the correct size
    // TODO: What do we do when frame is owned by frame object?
    // can we just make it owned by thread by construction?
    // TODO: Should we just make it owned by the frame object?
    int slots = code->co_nlocalsplus + code->co_stacksize;
    PyFrameObject *frame = PyObject_GC_NewVar(PyFrameObject, &PyFrame_Type, slots);
    init_frame(frame, code, frame_obj);

    return frame;
}

static void init_pyinterpreterframe(migrames::PyInterpreterFrame *interp_frame, 
                                   serdes::DeserializedPyInterpreterFrame& frame_obj,
                                   py_weakref<PyFrameObject> frame,
                                   py_weakref<PyCodeObject> code) {
    interp_frame->f_globals = NULL;
    interp_frame->f_builtins = NULL;
    interp_frame->f_locals = NULL;
    interp_frame->previous = NULL;

    interp_frame->f_executable.bits = (uintptr_t)Py_NewRef(code.borrow());
    interp_frame->f_funcobj = Py_NewRef(frame_obj.f_funcobj.borrow());

    if(NULL != *frame_obj.f_globals) {
        interp_frame->f_globals = Py_NewRef(frame_obj.f_globals.borrow());
    } else {
        interp_frame->f_globals = PyEval_GetFrameGlobals();

    }
    if(NULL != *frame_obj.f_builtins) {
        interp_frame->f_builtins = Py_NewRef(frame_obj.f_builtins.borrow());
    } else {
        interp_frame->f_builtins = PyEval_GetFrameBuiltins();
    }
    
    // These are NOT fast locals, those come from localsplus
    if(NULL != *frame_obj.f_locals) {
        interp_frame->f_locals = Py_NewRef(frame_obj.f_locals.borrow());
    }

    // Here are the locals plus
    auto localsplus = frame_obj.localsplus;
    for(size_t i = 0; i < localsplus.size(); i++) {
        interp_frame->localsplus[i].bits = (intptr_t) Py_NewRef(localsplus[i].borrow());
    }

    for(size_t i = localsplus.size(); i < code->co_nlocalsplus; i++) {
        interp_frame->localsplus[i].bits = 0;
    }

    interp_frame->instr_ptr = (migrames::PyBitcodeInstruction*) 
        (utils::py::get_code_adaptive(code) + frame_obj.instr_offset/2);
    auto opcode = interp_frame->instr_ptr->opcode;
    interp_frame->return_offset = frame_obj.return_offset;
    interp_frame->stackpointer = interp_frame->localsplus + code->co_nlocalsplus;
    // TODO: Check what happens when we make the owner the frame object instead of the thread.
    // Might allow us to skip a copy when calling this frame
    interp_frame->owner = frame_obj.owner;
    interp_frame->frame_obj = (PyFrameObject*) Py_NewRef(frame.borrow());
    frame->f_frame = interp_frame;
}

static migrames::PyInterpreterFrame *create_pyinterpreterframe_object(serdes::DeserializedPyInterpreterFrame& frame_obj, 
                                                                      py_weakref<PyFrameObject> frame, 
                                                                      py_weakref<PyCodeObject> code
                                                                      ) {
    migrames::PyInterpreterFrame *interp_frame = NULL;
    interp_frame = AllocateFrameToMigrate(code->co_framesize);
    init_pyinterpreterframe(interp_frame, frame_obj, frame, code);
    return interp_frame;
}

static PyObject *_deserialize_frame(PyObject *bytes) {
    if(PyErr_Occurred()) {
        PyErr_Print();
        return NULL;
    }
    loads_functor loads(migrames_state->pickle_loads);
    dumps_functor dumps(migrames_state->pickle_dumps);
    serdes::PyObjectSerdes po_serdes(loads, dumps);
    serdes::PyFrameSerdes frame_serdes{po_serdes};

    uint8_t *data = (uint8_t *)PyBytes_AsString(bytes);

    auto serframe = pyframe_buffer::GetPyFrame(data);
    auto deserframe = frame_serdes.deserialize(serframe);
    utils::py::print_object(deserframe.f_frame.f_funcobj.borrow());

    // FRAME_OWNED_BY_THREAD
    assert(deserframe.f_frame.owner == 0);

    PyCodeObject *code = create_pycode_object(deserframe.f_frame.f_executable);
    PyFrameObject *frame = create_pyframe_object(deserframe, code);
    // TODO: Should we just create the interpreter frame on the framestack, instead of doing it here?
    create_pyinterpreterframe_object(deserframe.f_frame, frame, code);

    return (PyObject*) frame;
}


static PyObject *run_deserialized_frame(PyObject *self, PyObject *args) {
    PyFrameObject *frame = NULL;
    if (!PyArg_ParseTuple(args, "O", &frame)) {
        return NULL;
    }

    PyThreadState *tstate = PyThreadState_Get();
    PyCodeObject *code = PyFrame_GetCode(frame);
    PyFrameObject *to_run = push_frame_for_running(tstate, frame->f_frame, code);
    if (to_run == NULL) {
        PySys_WriteStderr("<Migrames>: failed to create frame on the framestack\n");
        return NULL;
    }
    PyObject *res = PyEval_EvalFrame(to_run);
    return res;
}

static PyObject *serialize_frame(PyObject *self, PyObject *args) {
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return NULL;
    }

    return _serialize_frame_from_capsule(capsule);
}

static PyObject *deserialize_frame(PyObject *self, PyObject *args) {
    PyObject *bytes;
    if (!PyArg_ParseTuple(args, "O", &bytes)) {
        return NULL;
    }

    return _deserialize_frame(bytes);
}

static PyMethodDef MyMethods[] = {
    {"copy_and_run_frame", copy_and_run_frame, METH_VARARGS, "Copy the current frame and run it"},
    {"copy_frame", copy_frame, METH_VARARGS, "Copy the current frame"},
    {"run_frame", run_frame, METH_VARARGS, "Run the frame from the capsule"},
    {"serialize_frame", serialize_frame, METH_VARARGS, "Serialize the frame"},
    {"deserialize_frame", deserialize_frame, METH_VARARGS, "Deserialize the frame"},
    {"run_deserialized_frame", run_deserialized_frame, METH_VARARGS, "Run the deserialized frame"},
    {NULL, NULL, 0, NULL}
};

static void migrames_free(void *m) {
    delete migrames_state;
}

static struct PyModuleDef migrames_mod = {
    PyModuleDef_HEAD_INIT,
    "migrames",
    "A module that defines the 'abcd' function",
    -1,
    MyMethods,
    NULL, // slot definitions
    NULL, // traverse function for GC
    NULL, // clear function for GC
    migrames_free // free function for GC
};

PyMODINIT_FUNC PyInit_migrames(void) {
    migrames_state = new migrames_modulestate();
    return PyModule_Create(&migrames_mod);
}

}