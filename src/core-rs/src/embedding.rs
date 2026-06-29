use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::path::PathBuf;

pub const EMBEDDING_DIM: usize = 32;

pub type FaceEmbedding = Vec<f32>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FaceEmbeddingError {
    EmptySource,
    EmptyEmbedding,
    InvalidEmbeddingValue,
    InvalidImageSource,
}

#[derive(Debug, Clone, PartialEq)]
pub enum FaceSample {
    Mock { id: String },
    ImageFile { path: PathBuf },
    CameraFrame { descriptor: String },
    PrecomputedEmbedding { embedding: FaceEmbedding },
}

impl FaceSample {
    fn validate_source(&self) -> Result<(), FaceEmbeddingError> {
        match self {
            Self::Mock { .. } | Self::CameraFrame { .. } | Self::PrecomputedEmbedding { .. } => {
                Ok(())
            }
            Self::ImageFile { path } if path.is_file() && has_supported_image_extension(path) => {
                Ok(())
            }
            Self::ImageFile { .. } => Err(FaceEmbeddingError::InvalidImageSource),
        }
    }

    pub fn stable_mock_key(&self) -> String {
        match self {
            Self::Mock { id } => format!("mock:{id}"),
            Self::ImageFile { path } => format!("image:{}", path.display()),
            Self::CameraFrame { descriptor } => format!("camera:{descriptor}"),
            Self::PrecomputedEmbedding { embedding } => format!("embedding:{:.6?}", embedding),
        }
    }
}

pub(crate) fn parse_face_sample_source(source: &str) -> Result<FaceSample, FaceEmbeddingError> {
    let source = source.trim();
    if source.is_empty() {
        return Err(FaceEmbeddingError::EmptySource);
    }

    if let Some(value) = source.strip_prefix("mock:") {
        return non_empty(value)
            .map(|id| FaceSample::Mock { id })
            .ok_or(FaceEmbeddingError::EmptySource);
    }
    if let Some(value) = source.strip_prefix("image:") {
        return non_empty(value)
            .map(|path| FaceSample::ImageFile {
                path: PathBuf::from(path),
            })
            .ok_or(FaceEmbeddingError::EmptySource);
    }
    if let Some(value) = source.strip_prefix("camera:") {
        return non_empty(value)
            .map(|descriptor| FaceSample::CameraFrame { descriptor })
            .ok_or(FaceEmbeddingError::EmptySource);
    }
    if let Some(value) = source.strip_prefix("embedding:") {
        return parse_embedding_csv(value)
            .map(|embedding| FaceSample::PrecomputedEmbedding { embedding });
    }

    Ok(FaceSample::Mock {
        id: source.to_owned(),
    })
}

// Produce an embedding from a face sample source. Precomputed embeddings are
// passed through; everything else uses the deterministic mock generator. Real
// model inference lives on the C++ side (SeetaFace) and reaches Rust only as a
// precomputed embedding:... source, so there is no "model backend" here.
fn embed_sample(sample: &FaceSample) -> Result<FaceEmbedding, FaceEmbeddingError> {
    match sample {
        FaceSample::PrecomputedEmbedding { embedding } => Ok(embedding.clone()),
        _ => Ok(mock_embedding_from_source(&sample.stable_mock_key())),
    }
}

pub fn embedding_from_face_sample(sample_source: &str) -> Option<FaceEmbedding> {
    try_embedding_from_face_sample(sample_source).ok()
}

pub fn try_embedding_from_face_sample(
    sample_source: &str,
) -> Result<FaceEmbedding, FaceEmbeddingError> {
    let sample = parse_face_sample_source(sample_source)?;
    sample.validate_source()?;
    embed_sample(&sample)
}

fn mock_embedding_from_source(sample_source: &str) -> FaceEmbedding {
    let mut embedding = vec![0.0; EMBEDDING_DIM];
    for (index, value) in embedding.iter_mut().enumerate() {
        let mut hasher = DefaultHasher::new();
        sample_source.hash(&mut hasher);
        index.hash(&mut hasher);
        let raw = hasher.finish();
        *value = ((raw % 20_000) as f32 / 10_000.0) - 1.0;
    }
    normalize(embedding)
}

fn non_empty(value: &str) -> Option<String> {
    let value = value.trim();
    if value.is_empty() {
        return None;
    }
    Some(value.to_owned())
}

fn has_supported_image_extension(path: &std::path::Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .map(|extension| {
            matches!(
                extension.to_ascii_lowercase().as_str(),
                "jpg" | "jpeg" | "png" | "bmp" | "webp"
            )
        })
        .unwrap_or(false)
}

fn parse_embedding_csv(value: &str) -> Result<FaceEmbedding, FaceEmbeddingError> {
    let values = value
        .split(',')
        .map(str::trim)
        .map(str::parse::<f32>)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| FaceEmbeddingError::InvalidEmbeddingValue)?;
    normalize_embedding(values)
}

pub fn cosine_similarity(left: &FaceEmbedding, right: &FaceEmbedding) -> f32 {
    if left.len() != right.len() || left.is_empty() {
        return 0.0;
    }

    left.iter()
        .zip(right.iter())
        .map(|(a, b)| a * b)
        .sum::<f32>()
        .clamp(-1.0, 1.0)
}

pub(crate) fn normalize(embedding: FaceEmbedding) -> FaceEmbedding {
    normalize_embedding(embedding).unwrap_or_default()
}

pub(crate) fn normalize_embedding(
    mut embedding: FaceEmbedding,
) -> Result<FaceEmbedding, FaceEmbeddingError> {
    if embedding.is_empty() {
        return Err(FaceEmbeddingError::EmptyEmbedding);
    }

    if embedding.iter().any(|value| !value.is_finite()) {
        return Err(FaceEmbeddingError::InvalidEmbeddingValue);
    }

    let norm = embedding
        .iter()
        .map(|value| value * value)
        .sum::<f32>()
        .sqrt();
    if norm <= f32::EPSILON {
        return Ok(embedding);
    }

    for value in &mut embedding {
        *value /= norm;
    }
    Ok(embedding)
}
