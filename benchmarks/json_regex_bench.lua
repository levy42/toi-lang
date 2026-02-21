local has_cjson, cjson = pcall(require, "cjson")
local json
if has_cjson then
  json = cjson
else
  local has_dkjson, dkjson = pcall(require, "dkjson")
  if not has_dkjson then
    error("lua benchmark requires cjson or dkjson")
  end
  json = {
    encode = dkjson.encode,
    decode = function(s)
      return dkjson.decode(s)
    end,
  }
end

local PAYLOAD = {
  name = "Ada",
  active = true,
  score = 98.5,
  tags = {"pua", "json", "perf"},
  nested = { retries = 3, timeout_ms = 250, ok = true },
  users = {
    { id = 1, role = "admin" },
    { id = 2, role = "staff" },
    { id = 3, role = "member" },
  }
}

local ENCODE_ITERS = 20000
local DECODE_ITERS = 20000
local REGEX_ITERS = 120000
local TEXT = "order-123 shipped\norder-456 pending\norder-789 shipped"
local PATTERN = "order%-(%d+)%s+(%a+)"

local function bench(name, fn)
  local start = os.clock()
  local result = fn()
  local elapsed = os.clock() - start
  print(string.format("%s: %.6fs", name, elapsed))
  return result, elapsed
end

local function bench_json_encode()
  local encoded = ""
  for _ = 1, ENCODE_ITERS do
    encoded = json.encode(PAYLOAD)
  end
  return encoded
end

local function bench_json_decode(encoded)
  local decoded = nil
  for _ = 1, DECODE_ITERS do
    decoded = json.decode(encoded)
  end
  return decoded
end

local function bench_regex_search()
  local total = 0
  for _ = 1, REGEX_ITERS do
    local id = string.match(TEXT, PATTERN)
    total = total + tonumber(id)
  end
  return total
end

local function bench_regex_replace()
  local out = ""
  for _ = 1, REGEX_ITERS do
    out = string.gsub(TEXT, PATTERN, "id=%1 status=%2")
  end
  return #out
end

print("Lua json/regex benchmark")
print(string.format("iterations: encode=%d decode=%d regex=%d", ENCODE_ITERS, DECODE_ITERS, REGEX_ITERS))
local encoded = select(1, bench("json encode", bench_json_encode))
local decoded = select(1, bench("json decode", function() return bench_json_decode(encoded) end))
local regex_total = select(1, bench("regex search", bench_regex_search))
local replace_len = select(1, bench("regex replace", bench_regex_replace))
print(string.format("payload bytes: %d", #encoded))
print(string.format(
  "results: decoded_name=%s decoded_users=%d regex_total=%d replace_len=%d",
  decoded.name,
  #decoded.users,
  regex_total,
  replace_len
))
