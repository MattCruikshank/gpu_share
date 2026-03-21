fn main() -> Result<(), Box<dyn std::error::Error>> {
    tonic_prost_build::configure()
        .compile_with_config(
            tonic_prost_build::Config::new(),
            &["../proto/gpu_share.proto"],
            &["../proto"],
        )?;
    Ok(())
}
