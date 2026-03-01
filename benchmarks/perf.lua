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

local function bench_random()
  local acc = 0
  for i = 1, 2000000 do
    acc = acc + math.random()
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

local function bench_app_pipeline()
  local methods = {"GET", "POST", "PUT", "DELETE"}
  local paths = {"/users", "/teams", "/search", "/assets", "/health", "/events"}

  local routes = {
    ["GET /users"] = 1,
    ["POST /users"] = 2,
    ["GET /teams"] = 3,
    ["PUT /teams"] = 4,
    ["GET /search"] = 5,
    ["GET /assets"] = 6,
    ["GET /health"] = 7,
    ["POST /events"] = 8,
  }

  local cache = {}
  local total = 0
  for i = 1, 300000 do
    local method = methods[(i % 4) + 1]
    local path = paths[(i % 6) + 1]
    local key = method .. " " .. path

    local route = routes[key] or 0
    local auth = (i % 3) ~= 0
    local user_id = i % 10000
    local team_id = i % 100
    local body_size = (i * 37) % 2048

    local status = 200
    if (not auth) and path ~= "/health" then
      status = 401
    elseif method == "POST" and body_size < 32 then
      status = 422
    elseif route == 0 then
      status = 404
    end

    local cache_key = path .. ":" .. (user_id % 32) .. ":" .. team_id
    local cached = cache[cache_key]
    if cached ~= nil then
      total = total + cached
    else
      local cache_value = route + status + body_size
      cache[cache_key] = cache_value
      total = total + cache_value
    end

    total = total + status + route + user_id + team_id
  end

  return total
end

print("Lua perf:")
bench("math", bench_math)
bench("random", bench_random)
bench("string concat", bench_string_concat)
bench("string ops", bench_string_ops)
bench("table", bench_table)
bench("calls", bench_calls)
bench("app pipeline", bench_app_pipeline)
