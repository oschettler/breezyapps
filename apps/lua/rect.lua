mode(SM_VGA13H)
cls(0)
flip()

while not btn(KEY_ESC) do
    local x = rnd(300)
    local y = rnd(190)
    local w = rnd(80) + 10
    local h = rnd(60) + 10
    local c = rnd(15) + 1
    rect(x, y, w, h, c)
    flip()
    sleep(30)
end

mode(SM_TEXT)
cls(0)
