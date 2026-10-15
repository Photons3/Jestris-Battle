// Package sim runs the C++ game core (compiled to WASM) under wazero so the
// server can simulate every player's game.
package sim

import (
	"context"
	_ "embed"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
)

//go:embed jestris_sim.wasm
var wasm []byte

// LogCode values, matching enum class LogCode in src/net/protocol.hpp.
const (
	CodeLeft = iota
	CodeRight
	CodeRotateCW
	CodeRotateCCW
	CodeHardDrop
	CodeHold
	CodeLeftWall
	CodeRightWall
	CodeSoftOn
	CodeSoftOff
)

var tokenCodes = map[string]int{
	"L": CodeLeft, "R": CodeRight, "CW": CodeRotateCW, "CCW": CodeRotateCCW,
	"HD": CodeHardDrop, "H": CodeHold, "LW": CodeLeftWall, "RW": CodeRightWall,
	"S+": CodeSoftOn, "S-": CodeSoftOff,
}

// ParseToken decodes a wire token. Input tokens return their LogCode;
// "G<id>x<lines>" returns code -1 with the garbage id (>= 1) and lines (1-40).
func ParseToken(tok string) (code int, garbageID, garbageLines int, ok bool) {
	if c, found := tokenCodes[tok]; found {
		return c, 0, 0, true
	}
	rest, found := strings.CutPrefix(tok, "G")
	if !found {
		return 0, 0, 0, false
	}
	a, b, found := strings.Cut(rest, "x")
	if !found {
		return 0, 0, 0, false
	}
	id, err1 := strconv.Atoi(a)
	lines, err2 := strconv.Atoi(b)
	if err1 != nil || err2 != nil || id < 1 || lines < 1 || lines > 40 {
		return 0, 0, 0, false
	}
	return -1, id, lines, true
}

// Engine holds the wazero runtime and the compiled module, shared by matches.
type Engine struct {
	rt       wazero.Runtime
	compiled wazero.CompiledModule
}

func NewEngine(ctx context.Context) (*Engine, error) {
	rt := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfigCompiler())
	if _, err := wasi_snapshot_preview1.Instantiate(ctx, rt); err != nil {
		rt.Close(ctx)
		return nil, err
	}
	compiled, err := rt.CompileModule(ctx, wasm)
	if err != nil {
		rt.Close(ctx)
		return nil, err
	}
	return &Engine{rt: rt, compiled: compiled}, nil
}

func (e *Engine) Close(ctx context.Context) error { return e.rt.Close(ctx) }

// Match is one WASM instance holding any number of games, addressed by handle.
// It is not safe for concurrent use. After a trap every method returns an error.
type Match struct {
	ctx    context.Context
	mod    api.Module
	fns    map[string]api.Function
	bufPtr uint32
	err    error // poison
}

var exportNames = []string{"sim_new", "sim_free", "sim_input", "sim_accept_garbage", "sim_advance", "sim_t",
	"sim_take_attack", "sim_top_outs", "sim_eliminated", "sim_hash", "sim_snapshot", "sim_buffer"}

func (e *Engine) NewMatch(ctx context.Context) (*Match, error) {
	cfg := wazero.NewModuleConfig().WithName("").WithStartFunctions()
	mod, err := e.rt.InstantiateModule(ctx, e.compiled, cfg)
	if err != nil {
		return nil, err
	}
	m := &Match{ctx: ctx, mod: mod, fns: map[string]api.Function{}}
	if init := mod.ExportedFunction("_initialize"); init != nil {
		if _, err := init.Call(ctx); err != nil {
			mod.Close(ctx)
			return nil, fmt.Errorf("sim: _initialize: %w", err)
		}
	}
	for _, n := range exportNames {
		f := mod.ExportedFunction(n)
		if f == nil {
			mod.Close(ctx)
			return nil, fmt.Errorf("sim: missing export %s", n)
		}
		m.fns[n] = f
	}
	r, err := m.fns["sim_buffer"].Call(ctx)
	if err != nil {
		mod.Close(ctx)
		return nil, err
	}
	m.bufPtr = uint32(r[0])
	return m, nil
}

func (m *Match) call(name string, args ...uint64) (uint64, error) {
	if m.err != nil {
		return 0, m.err
	}
	r, err := m.fns[name].Call(m.ctx, args...)
	if err != nil {
		m.err = fmt.Errorf("sim: %s: %w", name, err)
		return 0, m.err
	}
	if len(r) == 0 {
		return 0, nil
	}
	return r[0], nil
}

func i32(v int) uint64 { return api.EncodeI32(int32(v)) }

func (m *Match) callInt(name string, args ...uint64) (int, error) {
	r, err := m.call(name, args...)
	return int(api.DecodeI32(r)), err
}

func (m *Match) New(seed uint32, level, players, sdf int) (int, error) {
	return m.callInt("sim_new", api.EncodeU32(seed), i32(level), i32(players), i32(sdf))
}

func (m *Match) Input(h, code int) error {
	_, err := m.call("sim_input", i32(h), i32(code))
	return err
}

func (m *Match) AcceptGarbage(h, id, lines int) error {
	_, err := m.call("sim_accept_garbage", i32(h), i32(id), i32(lines))
	return err
}

func (m *Match) Advance(h, ms int) error {
	_, err := m.call("sim_advance", i32(h), i32(ms))
	return err
}

func (m *Match) T(h int) (int, error)          { return m.callInt("sim_t", i32(h)) }
func (m *Match) TakeAttack(h int) (int, error) { return m.callInt("sim_take_attack", i32(h)) }
func (m *Match) TopOuts(h int) (int, error)    { return m.callInt("sim_top_outs", i32(h)) }

func (m *Match) Eliminated(h int) (bool, error) {
	v, err := m.callInt("sim_eliminated", i32(h))
	return v != 0, err
}

func (m *Match) Hash(h int) (uint64, error) { return m.call("sim_hash", i32(h)) }

func (m *Match) Snapshot(h int) (string, error) {
	n, err := m.callInt("sim_snapshot", i32(h))
	if err != nil {
		return "", err
	}
	b, ok := m.mod.Memory().Read(m.bufPtr, uint32(n))
	if n < 0 || !ok {
		m.err = errors.New("sim: snapshot out of range")
		return "", m.err
	}
	return string(b), nil
}

// Close releases the instance; further calls return an error.
func (m *Match) Close() error {
	if m.err == nil {
		m.err = errors.New("sim: match closed")
	}
	return m.mod.Close(m.ctx)
}
