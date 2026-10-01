package lunardyson

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"
)

type conformanceCase struct {
	Name    string         `json:"name"`
	Limits  map[string]any `json:"limits"`
	Tools   []toolSpec     `json:"tools"`
	Source  string         `json:"source"`
	Input   any            `json:"input"`
	Context any            `json:"context"`
	Expect  expectation    `json:"expect"`
	Then    *struct {
		Source string      `json:"source"`
		Expect expectation `json:"expect"`
	} `json:"then"`
}

type toolSpec struct {
	Name               string  `json:"name"`
	Effect             string  `json:"effect"`
	MaxCalls           *uint32 `json:"max_calls"`
	MaxResponseBytes   uint64  `json:"max_response_bytes"`
	Returns            any     `json:"returns"`
	ReturnsStringBytes int     `json:"returns_string_bytes"`
}

type expectation struct {
	ErrorKind       []string  `json:"error_kind"`
	OutputEquals    any       `json:"output_equals"`
	ToolCalls       *uint32   `json:"tool_calls"`
	VMTimeMsBetween []float64 `json:"vm_time_ms_between"`
}

func newCaseRuntime(t *testing.T, c conformanceCase) *Runtime {
	limits := &Limits{}
	if v, ok := c.Limits["memory_mb"].(float64); ok {
		limits.MemoryBytes = uint64(v * 1024 * 1024)
	}
	if v, ok := c.Limits["cpu_time_ms"].(float64); ok {
		limits.CPUTimeMs = uint32(v)
	}
	if m, ok := c.Limits["max_effects"].(map[string]any); ok {
		limits.MaxEffects = map[string]uint32{}
		for k, v := range m {
			limits.MaxEffects[k] = uint32(v.(float64))
		}
	}
	rt, err := NewRuntime(limits)
	if err != nil {
		t.Fatal(err)
	}
	for _, spec := range c.Tools {
		value := spec.Returns
		if spec.ReturnsStringBytes > 0 {
			value = strings.Repeat("x", spec.ReturnsStringBytes)
		}
		opts := ToolOptions{Effect: spec.Effect, MaxCalls: spec.MaxCalls, MaxResponseBytes: spec.MaxResponseBytes}
		if err := rt.Tool(spec.Name, func(any) (any, error) { return value, nil }, opts); err != nil {
			t.Fatal(err)
		}
	}
	return rt
}

func checkExpectation(t *testing.T, r Result, e expectation) {
	t.Helper()
	if len(e.ErrorKind) > 0 {
		if r.OK || !slices.Contains(e.ErrorKind, r.ErrorKind) {
			t.Fatalf("expected error %v, got ok=%v kind=%s msg=%s output=%v", e.ErrorKind, r.OK, r.ErrorKind, r.Message, r.Output)
		}
	}
	if e.OutputEquals != nil {
		if !r.OK {
			t.Fatalf("expected output, got %s: %s", r.ErrorKind, r.Message)
		}
		if !reflect.DeepEqual(r.Output, e.OutputEquals) {
			t.Fatalf("output %v != %v", r.Output, e.OutputEquals)
		}
	}
	if e.ToolCalls != nil && r.Stats.ToolCalls != *e.ToolCalls {
		t.Fatalf("tool calls %d != %d", r.Stats.ToolCalls, *e.ToolCalls)
	}
	if len(e.VMTimeMsBetween) == 2 {
		ms := float64(r.Stats.VMTimeUs) / 1000
		if ms < e.VMTimeMsBetween[0] || ms > e.VMTimeMsBetween[1] {
			t.Fatalf("vm time %.1fms outside %v", ms, e.VMTimeMsBetween)
		}
	}
}

func TestConformance(t *testing.T) {
	data, err := os.ReadFile(filepath.Join("..", "..", "tests", "conformance", "cases.json"))
	if err != nil {
		t.Fatal(err)
	}
	var suite struct {
		Cases []conformanceCase `json:"cases"`
	}
	if err := json.Unmarshal(data, &suite); err != nil {
		t.Fatal(err)
	}
	for _, c := range suite.Cases {
		t.Run(c.Name, func(t *testing.T) {
			rt := newCaseRuntime(t, c)
			defer rt.Close()
			r, err := rt.Execute(c.Source, c.Input, c.Context)
			if err != nil {
				t.Fatal(err)
			}
			checkExpectation(t, r, c.Expect)
			if c.Then != nil {
				r, _ := rt.Execute(c.Then.Source, nil, nil)
				checkExpectation(t, r, c.Then.Expect)
			}
		})
	}
}

// Smoke test proving the runtime is host-language agnostic: the T3 order
// fulfillment reference from the RiteSmith benchmark, with its tools in Go.
const orderFulfillment = `
local function firstShortage(order)
    for _, item in order.items do
        local stock = tools.inventory.check({ sku = item.sku })
        if not stock.ok or stock.available < item.qty then
            return item.sku
        end
    end
    return nil
end

function run(input, context)
    local shipped, blocked = {}, {}
    for _, id in input.order_ids do
        local r = tools.orders.get({ id = id })
        if not r.ok then
            table.insert(blocked, { order_id = id, reason = "not_found" })
        elseif r.order.status ~= "paid" then
            table.insert(blocked, { order_id = id, reason = "not_paid" })
        else
            local sku = firstShortage(r.order)
            if sku then
                table.insert(blocked, { order_id = id, reason = "out_of_stock:" .. sku })
            else
                local s = tools.shipping.create({ order_id = id })
                table.insert(shipped, { order_id = id, tracking = s.tracking })
            end
        end
    end
    return { shipped = shipped, blocked = blocked }
end`

func TestOrderFulfillmentWithGoTools(t *testing.T) {
	type item = map[string]any
	orders := map[string]item{
		"o1": {"id": "o1", "status": "paid", "items": []any{item{"sku": "A", "qty": 2}, item{"sku": "B", "qty": 1}}},
		"o2": {"id": "o2", "status": "pending", "items": []any{item{"sku": "A", "qty": 1}}},
		"o3": {"id": "o3", "status": "paid", "items": []any{item{"sku": "A", "qty": 1}, item{"sku": "C", "qty": 5}}},
		"o4": {"id": "o4", "status": "paid", "items": []any{item{"sku": "B", "qty": 3}}},
	}
	stock := map[string]float64{"A": 10, "B": 3, "C": 2}
	shipments := 0

	rt, _ := NewRuntime(nil)
	defer rt.Close()
	must := func(err error) {
		if err != nil {
			t.Fatal(err)
		}
	}
	must(rt.Tool("orders.get", func(args any) (any, error) {
		o, ok := orders[args.(map[string]any)["id"].(string)]
		if !ok {
			return item{"ok": false, "error": "not_found", "message": "no such order"}, nil
		}
		return item{"ok": true, "order": o}, nil
	}, ToolOptions{}))
	must(rt.Tool("inventory.check", func(args any) (any, error) {
		return item{"ok": true, "available": stock[args.(map[string]any)["sku"].(string)]}, nil
	}, ToolOptions{}))
	must(rt.Tool("shipping.create", func(args any) (any, error) {
		shipments++
		return item{"ok": true, "tracking": fmt.Sprintf("TRK-%s", args.(map[string]any)["order_id"])}, nil
	}, ToolOptions{Effect: "external_write"}))

	r, err := rt.Execute(orderFulfillment, item{"order_ids": []string{"o1", "o2", "o3", "o9", "o4"}}, nil)
	must(err)
	if !r.OK {
		t.Fatalf("%s: %s", r.ErrorKind, r.Message)
	}
	want := map[string]any{
		"shipped": []any{
			map[string]any{"order_id": "o1", "tracking": "TRK-o1"},
			map[string]any{"order_id": "o4", "tracking": "TRK-o4"},
		},
		"blocked": []any{
			map[string]any{"order_id": "o2", "reason": "not_paid"},
			map[string]any{"order_id": "o3", "reason": "out_of_stock:C"},
			map[string]any{"order_id": "o9", "reason": "not_found"},
		},
	}
	if !reflect.DeepEqual(r.Output, want) {
		t.Fatalf("output %v", r.Output)
	}
	if shipments != 2 {
		t.Fatalf("expected 2 shipments, got %d", shipments)
	}
}

func TestCheckFromGo(t *testing.T) {
	rt, _ := NewRuntime(nil)
	defer rt.Close()
	_ = rt.DeclareTypes("type Price = { ok: true, price: number } | { ok: false, error: string }")
	_ = rt.Tool("market.get_price", func(any) (any, error) { return nil, nil },
		ToolOptions{Signature: "(args: { symbol: string }) -> Price"})
	diags, err := rt.Check("function run() local r = tools.market.get_price({ symbol = 'BTC' }) return { p = r.price } end", true)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, d := range diags {
		if d.Severity == "error" && d.Kind == "TypeError" {
			found = true
		}
	}
	if !found {
		t.Fatalf("expected a TypeError for unrefined union access, got %+v", diags)
	}
}
