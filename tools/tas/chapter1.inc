# chapter 1: play until the boss is beaten and chapter 2 begins
until u32 chapter == 0 timeout 10
log chapter 1
until f32 bosshp > 0 timeout 300
log chapter 1 boss: lives {u8 lives}
until u32 chapter == 1 timeout 240
log chapter 1 cleared: lives {u8 lives}, shots {u32 shots}, hits {u32 hits}
check u8 lives > 0
