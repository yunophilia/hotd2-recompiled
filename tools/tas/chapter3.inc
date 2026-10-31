# chapter 3: play until the boss is beaten and chapter 4 begins
until u32 chapter == 2 timeout 10
log chapter 3
until f32 bosshp > 0 timeout 300
log chapter 3 boss: lives {u8 lives}
until u32 chapter == 3 timeout 240
log chapter 3 cleared: lives {u8 lives}, shots {u32 shots}, hits {u32 hits}
check u8 lives > 0
