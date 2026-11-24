-- For PCSX-Redux (tools/probe/emulator/run-pcsx-redux.sh): writes the
-- emulator's own copy of VRAM to JBP_VRAM_DIR/vram-N.bin each time the
-- probe ends session N, that is when it calls hal_show (whose address
-- run-pcsx-redux.sh passes in JBP_HAL_SHOW) for the second time and after.
-- hal_show is called once at boot, then after each session, after the
-- results have been sent and before it draws its idle screen; so the file
-- is VRAM as the probe read it back, taken independently of the probe and
-- its serial line. Needs -interpreter -debugger (breakpoints).
local dir = JBP_VRAM_DIR or "."
local calls = 0
jbp_breakpoint = PCSX.addBreakpoint(JBP_HAL_SHOW, 'Exec', 4, 'probe: session ended', function()
  calls = calls + 1
  if calls > 1 then
    local path = string.format("%s/vram-%d.bin", dir, calls - 1)
    local f = Support.File.open(path, "TRUNCATE")
    f:write(PCSX.GPU.getVRAM())
    f:close()
    print("jbp: wrote " .. path)
  end
end)
print(string.format("jbp: watching hal_show at 0x%08x", JBP_HAL_SHOW))
