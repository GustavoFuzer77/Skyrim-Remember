use std::collections::HashMap;

/// Identity of one actor reference in the current game session.
///
/// The C++ adapter is responsible for extracting and later resolving the
/// corresponding Skyrim reference FormID.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NpcId(u32);

impl NpcId {
    pub fn new(reference_form_id: u32) -> Option<Self> {
        (reference_form_id != 0).then_some(Self(reference_form_id))
    }

    pub fn get(self) -> u32 {
        self.0
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct NpcMemory {
    pub recognition: f32,
    pub suspicion: f32,
    pub fear: f32,
    pub respect: f32,
}

#[derive(Debug, Default)]
pub struct MemoryStore {
    memories: HashMap<NpcId, NpcMemory>,
}

impl MemoryStore {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn len(&self) -> usize {
        self.memories.len()
    }

    pub fn is_empty(&self) -> bool {
        self.memories.is_empty()
    }

    pub fn get(&self, npc_id: NpcId) -> Option<&NpcMemory> {
        self.memories.get(&npc_id)
    }

    pub fn get_or_create(&mut self, npc_id: NpcId) -> &mut NpcMemory {
        self.memories.entry(npc_id).or_default()
    }

    pub fn remove(&mut self, npc_id: NpcId) -> Option<NpcMemory> {
        self.memories.remove(&npc_id)
    }

    pub fn clear(&mut self) {
        self.memories.clear();
    }
}
