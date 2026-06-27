use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::path::PathBuf;

pub const EMBEDDING_DIM: usize = 32;

pub type FaceEmbedding = [f32; EMBEDDING_DIM];

#[derive(Debug, Clone, PartialEq)]
pub enum FaceSample {
    Mock { id: String },
    ImageFile { path: PathBuf },
    CameraFrame { descriptor: String },
    PrecomputedEmbedding { embedding: FaceEmbedding },
}

impl FaceSample {
    pub fn from_source(source: &str) -> Option<Self> {
        let source = source.trim();
        if source.is_empty() {
            return None;
        }

        if let Some(value) = source.strip_prefix("mock:") {
            return non_empty(value).map(|id| Self::Mock { id });
        }
        if let Some(value) = source.strip_prefix("image:") {
            return non_empty(value).map(|path| Self::ImageFile {
                path: PathBuf::from(path),
            });
        }
        if let Some(value) = source.strip_prefix("camera:") {
            return non_empty(value).map(|descriptor| Self::CameraFrame { descriptor });
        }
        if let Some(value) = source.strip_prefix("embedding:") {
            return parse_embedding_csv(value)
                .map(|embedding| Self::PrecomputedEmbedding { embedding });
        }

        Some(Self::Mock {
            id: source.to_owned(),
        })
    }

    pub fn validate(&self) -> bool {
        match self {
            Self::Mock { .. } | Self::CameraFrame { .. } | Self::PrecomputedEmbedding { .. } => {
                true
            }
            Self::ImageFile { path } => path.is_file() && has_supported_image_extension(path),
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

pub trait EmbeddingBackend {
    fn embed(&self, sample: &FaceSample) -> FaceEmbedding;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct MockEmbeddingBackend;

impl EmbeddingBackend for MockEmbeddingBackend {
    fn embed(&self, sample: &FaceSample) -> FaceEmbedding {
        match sample {
            FaceSample::PrecomputedEmbedding { embedding } => *embedding,
            _ => mock_embedding_from_source(&sample.stable_mock_key()),
        }
    }
}

pub fn default_embedding_backend() -> MockEmbeddingBackend {
    MockEmbeddingBackend
}

pub fn embedding_from_face_sample(sample_source: &str) -> Option<FaceEmbedding> {
    let sample = FaceSample::from_source(sample_source)?;
    if !sample.validate() {
        return None;
    }
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

fn parse_embedding_csv(value: &str) -> Option<FaceEmbedding> {
    let values = value
        .split(',')
        .map(str::trim)
        .map(str::parse::<f32>)
        .collect::<Result<Vec<_>, _>>()
        .ok()?;
    if values.len() != EMBEDDING_DIM {
        return None;
    }

    let embedding: FaceEmbedding = values.try_into().ok()?;
    Some(normalize(embedding))
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
