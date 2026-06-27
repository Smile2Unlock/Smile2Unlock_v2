use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};

pub const EMBEDDING_DIM: usize = 32;

pub type FaceEmbedding = [f32; EMBEDDING_DIM];

pub fn mock_embedding_from_sample(sample_seed: &str) -> FaceEmbedding {
    let mut embedding = [0.0; EMBEDDING_DIM];
    for (index, value) in embedding.iter_mut().enumerate() {
        let mut hasher = DefaultHasher::new();
        sample_seed.hash(&mut hasher);
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
            mock_embedding_from_sample("face:alice:front"),
            mock_embedding_from_sample("face:alice:front")
        );
    }

    #[test]
    fn cosine_similarity_is_one_for_same_embedding() {
        let embedding = mock_embedding_from_sample("face:alice:front");
        assert!((cosine_similarity(&embedding, &embedding) - 1.0).abs() < 0.0001);
    }
}
