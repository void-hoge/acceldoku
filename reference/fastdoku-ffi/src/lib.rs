// The caller provides valid 81-byte input and output arrays and a limit of 1 or 2.
#[no_mangle]
pub unsafe extern "C" fn fades_fastdoku_solve(
    input: *const u8,
    output: *mut u8,
    limit: u32,
) -> u32 {
    let clues = &*(input as *const [u8; 81]);
    let count = if limit == 1 {
        match fastdoku::solve_grid(clues) {
            Some(solution) => {
                std::ptr::copy_nonoverlapping(solution.as_ptr(), output, 81);
                return 1;
            }
            None => return 0,
        }
    } else {
        fastdoku::count_solutions(clues, u64::from(limit)) as u32
    };
    if count != 0 {
        if let Some(solution) = fastdoku::solve_grid(clues) {
            std::ptr::copy_nonoverlapping(solution.as_ptr(), output, 81);
        } else {
            std::process::abort();
        }
    }
    count
}
