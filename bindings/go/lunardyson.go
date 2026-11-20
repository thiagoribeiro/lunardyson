// Package lunardyson is the Go binding for LunarDyson, an execution runtime for
// untrusted Luau programs that orchestrate tools.
//
// Execute runs a program and invokes registered Go tool functions; Start gives
// the step API (the execution stops at every tool call until Resume).
// A Runtime is not safe for concurrent use; use one per goroutine or a pool.
package lunardyson

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -llunardyson -Wl,-rpath,${SRCDIR}/../../build
#include <stdlib.h>
#include <string.h>
#include "lunardyson.h"
*/
import "C"

import (
	"encoding/json"
	"errors"
	"fmt"
	"runtime"
	"unsafe"
)

const Unlimited = uint32(C.LD_UNLIMITED)

var errorKinds = []string{"none", "syntax", "timeout", "memory", "tool_budget", "effect_budget",
	"unknown_tool", "runtime", "contract", "invalid_arg"}

var effects = map[string]C.ld_effect{"pure": 0, "read": 1, "write": 2, "external_write": 3, "money": 4}

var effectNames = [...]string{"pure", "read", "write", "external_write", "money"}

var statusNames = [...]string{"done", "pending_tool", "error"}

// cstr points C at a Go string's bytes without copying. Only for calls that take an explicit
// length and do not keep the pointer (cgo allows Go memory without Go pointers in it).
func cstr(s string) *C.char {
	if s == "" {
		return (*C.char)(unsafe.Pointer(&zero))
	}
	return (*C.char)(unsafe.Pointer(unsafe.StringData(s)))
}

var zero byte

// cjson appends the NUL that the length-less C parameters need and points C at it (no extra copy).
func cjson(b []byte) *C.char {
	b = append(b, 0)
	return (*C.char)(unsafe.Pointer(&b[0]))
}

// view is a Go slice over C memory, valid only while that memory is (no copy).
func view(p *C.char, n C.size_t) []byte {
	if p == nil || n == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n))
}

// ToolFunc handles one tool call. A returned error is raised inside the program.
type ToolFunc func(args any) (any, error)

type Limits struct {
	MemoryBytes uint64
	CPUTimeMs   uint32
	MaxEffects  map[string]uint32
}

type ToolOptions struct {
	Signature        string
	Effect           string // pure | read (default) | write | external_write | money
	MaxCalls         *uint32
	MaxResponseBytes uint64
}

type Call struct {
	ID     uint64
	Tool   string
	Args   any
	Effect string
}

type Stats struct {
	VMTimeUs        uint64
	PeakMemoryBytes uint64
	ToolCalls       uint32
}

type Result struct {
	OK        bool
	Output    any
	ErrorKind string
	Message   string
	Stats     Stats
}

type Runtime struct {
	ptr   *C.ld_runtime
	tools map[string]ToolFunc
}

func Version() string { return C.GoString(C.ld_version()) }

func NewRuntime(limits *Limits) (*Runtime, error) {
	l := C.ld_limits_default()
	if limits != nil {
		if limits.MemoryBytes > 0 {
			l.memory_limit_bytes = C.size_t(limits.MemoryBytes)
		}
		if limits.CPUTimeMs > 0 {
			l.cpu_time_ms = C.uint32_t(limits.CPUTimeMs)
		}
		for name, max := range limits.MaxEffects {
			e, ok := effects[name]
			if !ok {
				return nil, fmt.Errorf("unknown effect %q", name)
			}
			l.max_effects[e] = C.uint32_t(max)
		}
	}
	ptr := C.ld_runtime_new(&l)
	if ptr == nil {
		return nil, errors.New("could not create runtime")
	}
	rt := &Runtime{ptr: ptr, tools: map[string]ToolFunc{}}
	runtime.SetFinalizer(rt, (*Runtime).Close)
	return rt, nil
}

func (rt *Runtime) Close() {
	if rt.ptr != nil {
		C.ld_runtime_free(rt.ptr)
		rt.ptr = nil
	}
}

func (rt *Runtime) Tool(name string, fn ToolFunc, opts ToolOptions) error {
	effect := "read"
	if opts.Effect != "" {
		effect = opts.Effect
	}
	e, ok := effects[effect]
	if !ok {
		return fmt.Errorf("unknown effect %q", effect)
	}
	maxCalls := Unlimited
	if opts.MaxCalls != nil {
		maxCalls = *opts.MaxCalls
	}
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	var csig *C.char
	if opts.Signature != "" {
		csig = C.CString(opts.Signature)
		defer C.free(unsafe.Pointer(csig))
	}
	if C.ld_tool_register(rt.ptr, cname, csig, e, C.uint32_t(maxCalls), C.size_t(opts.MaxResponseBytes)) != 0 {
		return fmt.Errorf("cannot register tool %q (invalid name, duplicate, or runtime sealed)", name)
	}
	rt.tools[name] = fn
	return nil
}

func (rt *Runtime) DeclareTypes(decls string) error {
	c := C.CString(decls)
	defer C.free(unsafe.Pointer(c))
	if C.ld_declare_types(rt.ptr, c) != 0 {
		return errors.New("cannot declare types after the runtime is sealed")
	}
	return nil
}

type Diagnostic struct {
	Line     int    `json:"line"`
	Column   int    `json:"column"`
	Kind     string `json:"kind"`
	Severity string `json:"severity"`
	Message  string `json:"message"`
}

func (rt *Runtime) Check(source string, strict bool) ([]Diagnostic, error) {
	s := C.int(0)
	if strict {
		s = 1
	}
	raw := C.ld_check(rt.ptr, cstr(source), C.size_t(len(source)), s)
	if raw == nil {
		return nil, errors.New("check failed")
	}
	defer C.ld_free(unsafe.Pointer(raw))
	var diags []Diagnostic
	err := json.Unmarshal(view(raw, C.strlen(raw)), &diags)
	return diags, err
}

// Execution is one program run, driven step by step.
type Execution struct {
	rt     *Runtime
	ptr    *C.ld_exec
	status C.ld_status
}

func (rt *Runtime) Start(source string, input, context any) (*Execution, error) {
	if input == nil {
		input = map[string]any{}
	}
	if context == nil {
		context = map[string]any{}
	}
	in, err := json.Marshal(input)
	if err != nil {
		return nil, err
	}
	ctx, err := json.Marshal(context)
	if err != nil {
		return nil, err
	}
	var out *C.ld_exec
	status := C.ld_exec_start(rt.ptr, cstr(source), C.size_t(len(source)), cjson(in), cjson(ctx), &out)
	if out == nil {
		return nil, errors.New("could not start execution")
	}
	return &Execution{rt: rt, ptr: out, status: status}, nil
}

func (ex *Execution) Status() string {
	return statusNames[ex.status]
}

func (ex *Execution) Pending() (Call, bool) {
	var c C.ld_call
	if C.ld_exec_pending(ex.ptr, &c) == 0 {
		return Call{}, false
	}
	var args any
	_ = json.Unmarshal(view(c.args_json, c.args_len), &args)
	return Call{ID: uint64(c.call_id), Tool: C.GoString(c.tool), Args: args, Effect: effectNames[c.effect]}, true
}

func (ex *Execution) Resume(result any) (string, error) {
	payload, err := json.Marshal(result)
	if err != nil {
		return ex.Status(), err
	}
	ex.status = C.ld_exec_resume(ex.ptr, (*C.char)(unsafe.Pointer(&payload[0])), C.size_t(len(payload)))
	return ex.Status(), nil
}

func (ex *Execution) ResumeError(message string) string {
	cm := C.CString(message)
	defer C.free(unsafe.Pointer(cm))
	ex.status = C.ld_exec_resume_error(ex.ptr, cm)
	return ex.Status()
}

func (ex *Execution) Result() Result {
	raw := C.ld_exec_stats(ex.ptr)
	stats := Stats{uint64(raw.vm_time_us), uint64(raw.peak_memory_bytes), uint32(raw.tool_calls)}
	if ex.status == C.LD_DONE {
		var n C.size_t
		out := C.ld_exec_output(ex.ptr, &n)
		var value any
		_ = json.Unmarshal(view(out, n), &value)
		return Result{OK: true, Output: value, ErrorKind: "none", Stats: stats}
	}
	var msg *C.char
	kind := C.ld_exec_error(ex.ptr, &msg)
	return Result{OK: false, ErrorKind: errorKinds[kind], Message: C.GoString(msg), Stats: stats}
}

func (ex *Execution) Close() {
	if ex.ptr != nil {
		C.ld_exec_free(ex.ptr)
		ex.ptr = nil
	}
}

// Execute runs the program to completion, calling the registered Go tool functions.
func (rt *Runtime) Execute(source string, input, context any) (Result, error) {
	ex, err := rt.Start(source, input, context)
	if err != nil {
		return Result{}, err
	}
	defer ex.Close()
	for ex.status == C.LD_PENDING_TOOL {
		call, _ := ex.Pending()
		fn := rt.tools[call.Tool]
		if fn == nil {
			ex.ResumeError("no Go handler registered for " + call.Tool)
			continue
		}
		value, err := fn(call.Args)
		if err != nil {
			ex.ResumeError(err.Error())
			continue
		}
		if _, err := ex.Resume(value); err != nil {
			ex.ResumeError("tool result is not JSON-serializable: " + err.Error())
		}
	}
	return ex.Result(), nil
}
