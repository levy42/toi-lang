local function bench(name, fn)
  local start = os.clock()
  local result = fn()
  local elapsed = os.clock() - start
  print(string.format("%s %.6f sec", name, elapsed))
  return result
end

local function bench_math()
  local acc = 0
  for i = 1, 2000000 do
    acc = acc + (i * 3.14159) / ((i % 97) + 1)
  end
  return acc
end

local function bench_string_concat()
  local s = ""
  for i = 1, 20000 do
    s = s .. "abc"
  end
  return #s
end

local function bench_string_ops()
  local s = "the quick brown fox jumps over the lazy dog"
  local total = 0
  for i = 1, 20000 do
    local x = string.upper(s)
    local y = string.lower(x)
    local z = string.sub(y, 5, 19)
    total = total + #z
  end
  return total
end

local function bench_table()
  local t = {0}
  for i = 1, 200000 do
    t[i] = i * 2
  end
  local total = 0
  for i = 1, 200000 do
    total = total + t[i]
  end
  return total
end

local function bench_calls()
  local function add(a, b)
    return a + b
  end
  local acc = 0
  for i = 1, 2000000 do
    acc = add(acc, i)
  end
  return acc
end

print("Lua perf:")
bench("math", bench_math)
bench("string concat", bench_string_concat)
bench("string ops", bench_string_ops)
bench("table", bench_table)
bench("calls", bench_calls)
