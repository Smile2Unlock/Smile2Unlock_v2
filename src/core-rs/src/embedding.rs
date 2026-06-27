use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::path::PathBuf;

pub const EMBEDDING_DIM: usize = 32;

pub type FaceEmbedding = [f32; EMBEDDING_DIM];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EmbeddingBackendKind {
    Mock,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EmbeddingBackendConfig {
    pub kind: EmbeddingBackendKind,
}

impl Default for EmbeddingBackendConfig {
    fn default() -> Self {
        Self {
            kind: EmbeddingBackendKind::Mock,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FaceEmbeddingError {
    EmptySource,
    InvalidEmbeddingLength { expected: usize, actual: usize },
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

pub trait EmbeddingBackend {
    fn embed(&self, sample: &FaceSample) -> Result<FaceEmbedding, FaceEmbeddingError>;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct MockEmbeddingBackend;

impl EmbeddingBackend for MockEmbeddingBackend {
    fn embed(&self, sample: &FaceSample) -> Result<FaceEmbedding, FaceEmbeddingError> {
        match sample {
            FaceSample::PrecomputedEmbedding { embedding } => Ok(*embedding),
            _ => Ok(mock_embedding_from_source(&sample.stable_mock_key())),
        }
    }
}

pub fn default_embedding_backend() -> MockEmbeddingBackend {
    MockEmbeddingBackend
}

pub fn default_embedding_backend_config() -> EmbeddingBackendConfig {
    EmbeddingBackendConfig::default()
}

pub fn embedding_from_face_sample(sample_source: &str) -> Option<FaceEmbedding> {
    try_embedding_from_face_sample(sample_source).ok()
}

pub fn try_embedding_from_face_sample(
    sample_source: &str,
) -> Result<FaceEmbedding, FaceEmbeddingError> {
    try_embedding_from_face_sample_with_config(sample_source, &default_embedding_backend_config())
}

pub fn try_embedding_from_face_sample_with_config(
    sample_source: &str,
    config: &EmbeddingBackendConfig,
) -> Result<FaceEmbedding, FaceEmbeddingError> {
    let sample = parse_face_sample_source(sample_source)?;
    sample.validate_source()?;
    match config.kind {
        EmbeddingBackendKind::Mock => default_embedding_backend().embed(&sample),
    }
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
    if values.len() != EMBEDDING_DIM {
        return Err(FaceEmbeddingError::InvalidEmbeddingLength {
            expected: EMBEDDING_DIM,
            actual: values.len(),
        });
    }

    let embedding: FaceEmbedding = values.try_into().map_err(|values: Vec<f32>| {
        FaceEmbeddingError::InvalidEmbeddingLength {
            expected: EMBEDDING_DIM,
            actual: values.len(),
        }
    })?;
    Ok(normalize(embedding))
}

pub fn cosine_similarity(left: &FaceEmbedding, right: &FaceEmbedding) -> f32 {
    left.iter()
        .zip(right.iter())
        .map(|(a, b)| a * b)
        .sum::<f32>()
        .clamp(-1.0, 1.0)
}

pub(crate) fn normalize(mut embedding: FaceEmbedding) -> FaceEmbedding {
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
