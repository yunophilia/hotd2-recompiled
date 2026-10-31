# chapter 4: play until the boss is beaten and chapter 5 begins
until u32 chapter == 3 timeout 10
log chapter 4
until f32 bosshp > 0 timeout 300
log chapter 4 boss: lives {u8 lives}
until u32 chapter == 4 timeout 240
log chapter 4 cleared: lives {u8 lives}, shots {u32 shots}, hits {u32 hits}
check u8 lives > 0
