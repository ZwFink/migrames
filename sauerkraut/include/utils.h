#ifndef UTILS_HH_INCLUDED
#define UTILS_HH_INCLUDED
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <opcode_ids.h>
#include "py_structs.h"
#include "pyref.h"
#include <map>

namespace utils {
    namespace py {
       int get_code_stacksize(PyCodeObject *code) {
            return code->co_stacksize;
       }

       int get_code_nlocalsplus(PyCodeObject *code) {
           return code->co_nlocalsplus;
       }

       int get_code_nlocals(PyCodeObject *code) {
           return code->co_nlocals;
       }

     migrames::PyBitcodeInstruction *get_code_adaptive(py_weakref<PyCodeObject> code) {
           return (migrames::PyBitcodeInstruction*) code->co_code_adaptive;
       }

       int get_iframe_localsplus_size(migrames::PyInterpreterFrame *iframe) {
           PyCodeObject *code = (PyCodeObject*) iframe->f_executable.bits;
           if(NULL == code) {
               return 0;
           }
           return get_code_nlocalsplus(code) + code->co_stacksize;
       }

        enum class Units { Bytes, Instructions };

        template <Units Unit>
        Py_ssize_t get_instr_offset(struct _frame *frame) {
            PyCodeObject *code = PyFrame_GetCode(frame);
            Py_ssize_t first_instr_addr = (Py_ssize_t) code->co_code_adaptive;
            Py_ssize_t current_instr_addr = (Py_ssize_t) frame->f_frame->instr_ptr;
            Py_ssize_t offset = current_instr_addr - first_instr_addr;

            if constexpr (Unit == Units::Bytes) {
                return offset;
            } else if constexpr (Unit == Units::Instructions) {
                return offset / 2; // Assuming each instruction is 2 bytes
            }
        }


        Py_ssize_t get_offset_for_skipping_call() {
            return 5 * sizeof(_CodeUnit);
        }

        void print_object(PyObject *obj) {
            if (obj == NULL) {
                printf("Error: NULL object passed\n");
                return;
            }
        
            PyObject *str = PyObject_Repr(obj);
            if (str == NULL) {
                PyErr_Print();
                return;
            }
        
            const char *c_str = PyUnicode_AsUTF8(str);
            if (c_str == NULL) {
                Py_DECREF(str);
                PyErr_Print();
                return;
            }
        
            printf("Object contents: %s\n", c_str);
            Py_DECREF(str);
        }
        
        void print_object_type_name(PyObject *obj) {
            if (obj == NULL) {
                printf("Error: NULL object\n");
                return;
            }
        
            PyObject *type = PyObject_Type(obj);
            if (type == NULL) {
                printf("Error: Could not get object type\n");
                PyErr_Print();
                return;
            }
        
            PyObject *type_name = PyObject_GetAttrString(type, "__name__");
            if (type_name == NULL) {
                printf("Error: Could not get type name\n");
                PyErr_Print();
                Py_DECREF(type);
                return;
            }
        
            const char *name = PyUnicode_AsUTF8(type_name);
            if (name == NULL) {
                printf("Error: Could not convert type name to string\n");
                PyErr_Print();
            } else {
                printf("Object type: %s\n", name);
            }
        
            Py_DECREF(type_name);
            Py_DECREF(type);
        }

        Py_ssize_t get_code_size(Py_ssize_t n_instructions) {
            return n_instructions * sizeof(_CodeUnit);
        }

        Py_ssize_t get_current_stack_depth(migrames::PyInterpreterFrame *iframe) {
            // WARNING: The stack pointer is most often NULL when 
            // we stop a running Python function. Unless the function is stopped on a
            // yield instruction (which will not happen in this library, as we are stopped
            // on a CALL, then the stack pointer will be NULL.
            // This is NOT the method
            // you should use when trying to get the stack depth of a running frame.
            // Use get_stack_depth(PyObject *) instead.
            assert(NULL != iframe->stackpointer);
            assert(iframe->stackpointer >= iframe->localsplus);
            PyCodeObject *code = (PyCodeObject*) iframe->f_executable.bits;
            auto n_localsplus = get_code_nlocalsplus(code);
            return (Py_ssize_t) (iframe->stackpointer - ( iframe->localsplus + n_localsplus));
        }

        Py_ssize_t get_stack_depth(PyObject *frame) {
            // we must analyze the code to determine the current stack depth.
            // iframe->stackpointer is rarely written to (e.g., with generators).
            // Therefore, we have to determine the stack depth by looking backwards
            // at the compiled bitcode.
            // TODO: This ignores the following constructs which affect the stack depth:
            // 1. Try/except blocks; 2. Calls.
            // We consider only loops.
            // migrames::PyCodeObject *code = (migrames::PyCodeObject*) iframe->f_executable.bits;
            migrames::PyFrame *py_frame = (migrames::PyFrame*) frame;
            migrames::PyInterpreterFrame *iframe = py_frame->f_frame;
            PyCodeObject *code = (PyCodeObject*) PyFrame_GetCode((PyFrameObject*)py_frame); //iframe->f_executable.bits;

            // divide by 2 to convert from bytes to instructions
            Py_ssize_t offset = get_instr_offset<Units::Instructions>(py_frame);
            PyObject *code_bytes = PyCode_GetCode(code);
            char *bitcode = PyBytes_AsString(code_bytes);
            Py_ssize_t n_instructions = Py_SIZE(code);
            migrames::PyBitcodeInstruction *first_instr = (migrames::PyBitcodeInstruction*) bitcode;
            migrames::PyBitcodeInstruction *instr = (migrames::PyBitcodeInstruction*) first_instr + offset;

            Py_ssize_t num_for = 0;
            Py_ssize_t num_end = 0;
            while((intptr_t)instr >= (intptr_t)first_instr) {
                switch(instr->opcode) {
                    case FOR_ITER:
                        num_for++;
                        break;
                    case END_FOR:
                        num_end++;
                        break;
                    default:
                        break;
                }
                instr--;
            }
            return num_for - num_end;
        }

        void print_opcodes_til_now(PyObject *frame) {
            migrames::PyFrame *py_frame = (migrames::PyFrame*) frame;
            migrames::PyInterpreterFrame *iframe = py_frame->f_frame;
            PyCodeObject *code = (PyCodeObject*) PyFrame_GetCode((PyFrameObject*)py_frame);
            migrames::PyBitcodeInstruction *first_instr = (migrames::PyBitcodeInstruction*) code->co_code_adaptive;
            migrames::PyBitcodeInstruction *instr = (migrames::PyBitcodeInstruction*) iframe->instr_ptr;
        }
        _PyStackRef *get_stack_base(migrames::PyInterpreterFrame *f) {
            return f->localsplus + ((PyCodeObject*)f->f_executable.bits)->co_nlocalsplus;
        }

        template<typename T>
        class _StackState {
            std::vector<T> state;

            public:
            _StackState(size_t max_size) {
                state.reserve(max_size);
            }
            _StackState() {
                state.reserve(10);
            }
            void add(T value) {
                state.push_back(value);
            }
            T& get(size_t index) {
                return state[index];
            }
            size_t size() {
                return state.size();
            }
        };

        using StackState = _StackState<PyObject*>;
        static StackState _get_stack_state_locals(struct _frame *frame, PyCodeObject *code, int stack_depth) {
            StackState state(stack_depth);
            _PyInterpreterFrame *iframe = (_PyInterpreterFrame*) frame->f_frame;

            #ifdef DEBUG
            _PyStackRef *frame_locals = iframe->localsplus;
            std::map<intptr_t, int> locals;
            for(int i = 0; i < code->co_nlocalsplus; i++) {
                PyObject *local = ((PyObject*) frame_locals[i].bits);
                if(NULL == local) {
                    continue;
                }
                locals[(intptr_t) local] = i;
            }
            #endif

            _PyStackRef *stack_pointer = iframe->localsplus + code->co_nlocalsplus;
            for(int i = 0; i < stack_depth; i++) {
                PyObject *stack_obj = (PyObject*) stack_pointer[i].bits;
                #ifdef DEBUG
                assert(NULL != stack_obj);
                if(locals.find((intptr_t) stack_obj) != locals.end()) {
                    PySys_WriteStderr("ISSUE: We found a local on the stack. Currently, we assume that's not possible.\n");
                }
                #endif
                state.add(stack_obj);
            }


            return state;
        }

        StackState get_stack_state(pyobject_weakref frame) {
            py_weakref<struct _frame> frame_obj{(struct _frame*) *frame};
            _PyInterpreterFrame *iframe = (_PyInterpreterFrame*) frame_obj->f_frame;
            PyCodeObject *code = (PyCodeObject*) iframe->f_executable.bits;
            auto stack_depth = get_stack_depth((PyObject*)*frame);
            auto state = _get_stack_state_locals(*frame_obj, code, stack_depth);
            return state;
        }

    }
}


#endif // UTILS_HH_INCLUDED