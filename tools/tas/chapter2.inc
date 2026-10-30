# chapter 2: play until the boss is beaten and chapter 3 begins
until u32 chapter == 1 timeout 10
log chapter 2
until f32 bosshp > 0 timeout 300
log chapter 2 boss: lives {u8 lives}
until u32 chapter == 2 timeout 240
log chapter 2 cleared: lives {u8 lives}, shots {u32 shots}, hits {u32 hits}
check u8 lives > 0
