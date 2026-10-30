# chapter 5 (the last): five boss fights, the last one the final boss, then the ending
until u32 chapter == 4 timeout 10
log chapter 5
until f32 bosshp > 0 timeout 300
log chapter 5 boss 1: lives {u8 lives}
until f32 bosshp == 0 timeout 240
log chapter 5 boss 1 beaten: lives {u8 lives}
until f32 bosshp > 0 timeout 300
log chapter 5 boss 2: lives {u8 lives}
until f32 bosshp == 0 timeout 240
log chapter 5 boss 2 beaten: lives {u8 lives}
until f32 bosshp > 0 timeout 300
log chapter 5 boss 3: lives {u8 lives}
until f32 bosshp == 0 timeout 240
log chapter 5 boss 3 beaten: lives {u8 lives}
until f32 bosshp > 0 timeout 300
log chapter 5 boss 4: lives {u8 lives}
until f32 bosshp == 0 timeout 240
log chapter 5 boss 4 beaten: lives {u8 lives}
until f32 bosshp > 0 timeout 300
log chapter 5 boss 5: lives {u8 lives}
until f32 bosshp == 0 timeout 240
log chapter 5 boss 5 beaten: lives {u8 lives}, shots {u32 shots}, hits {u32 hits}
check u8 lives > 0
