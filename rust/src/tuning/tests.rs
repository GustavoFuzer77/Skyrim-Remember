use super::*;

#[test]
fn a_stranger_needs_the_base_threshold() {
    let tuning = RecognitionTuning::default();

    assert!((tuning.threshold(0.0) - 0.70).abs() < 0.0001);
}

#[test]
fn full_familiarity_lowers_the_threshold_to_the_floor() {
    let tuning = RecognitionTuning::default();

    assert!((tuning.threshold(MAX_MEMORY_VALUE) - 0.50).abs() < 0.0001);
    assert!((tuning.threshold(MAX_MEMORY_VALUE / 2.0) - 0.60).abs() < 0.0001);
}

#[test]
fn out_of_range_recognition_stays_inside_the_threshold_band() {
    let tuning = RecognitionTuning::default();

    assert!((tuning.threshold(-50.0) - 0.70).abs() < 0.0001);
    assert!((tuning.threshold(1_000.0) - 0.50).abs() < 0.0001);
}

#[test]
fn weights_are_addressed_by_category_rather_than_by_index() {
    let weights = AppearanceWeights::default();

    assert_eq!(weights.get(AppearanceCategory::Body), 0.35);
    assert_eq!(weights.get(AppearanceCategory::Accessories), 0.05);
}
