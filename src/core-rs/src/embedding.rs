use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};

pub const EMBEDDING_DIM: usize = 32;

pub type FaceEmbedding = [f32; EMBEDDING_DIM];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FaceSample {
    pub source: String,
}

impl FaceSample {
    pub fn from_source(source: &str) -> Option<Self> {
        let source = source.trim();
        if source.is_empty() {
            return None;
        }
        Some(Self {
            source: source.to_owned(),
        })
    }
}

pub trait EmbeddingBackend {
    fn embed(&self, sample: &FaceSample) -> FaceEmbedding;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct MockEmbeddingBackend;

impl EmbeddingBackend for MockEmbeddingBackend {
    fn embed(&self, sample: &FaceSample) -> FaceEmbedding {
        mock_embedding_from_source(&sample.source)
    }
}

pub fn default_embedding_backend() -> MockEmbeddingBackend {
    MockEmbeddingBackend
}

pub fn embedding_from_face_sample(sample_source: &str) -> Option<FaceEmbedding> {
    let sample = FaceSample::from_source(sample_source)?;
    Some(default_embedding_backend().embed(&sample))
}

fn mock_embedding_from_source(sample_source: &str) -> FaceEmbedding {
    let mut embedding = [0.0; EMBEDDING_DIM];
    for (index, value) in embedding.iter_mut().enumerate() {
        let mut hasher = DefaultHasher::new();
        sample_source.hash(&mut hasher);
        index.hash(&mut hasher);
        let raw = hasher.finish();
        *value = ((raw % 20_000) as f32 / 10_000.0) - 1.0;
    }
    normalize(embedding)
}

pub fn cosine_similarity(left: &FaceEmbedding, right: &FaceEmbedding) -> f32 {
    left.iter()
        .zip(right.iter())
        .map(|(a, b)| a * b)
        .sum::<f32>()
        .clamp(-1.0, 1.0)
}

fn normalize(mut embedding: FaceEmbedding) -> FaceEmbedding {
    let norm = embedding
        .iter()
        .map(|value| value * value)
        .sum::<f32>()
        .sqrt();
    if norm <= f32::EPSILON {
        return embedding;
    }

    for value in &mut embedding {
        *value /= norm;
    }
    embedding
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_embeddings_are_deterministic() {
        assert_eq!(
            embedding_from_face_sample("face:alice:front"),
            embedding_from_face_sample("face:alice:front")
        );
    }

    #[test]
    fn cosine_similarity_is_one_for_same_embedding() {
        let embedding = embedding_from_face_sample("face:alice:front").unwrap();
        assert!((cosine_similarity(&embedding, &embedding) - 1.0).abs() < 0.0001);
    }

    #[test]
    fn rejects_empty_face_sample_source() {
        assert!(embedding_from_face_sample(" ").is_none());
    }
}
