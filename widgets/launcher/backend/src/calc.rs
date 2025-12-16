use evalexpr::eval;

pub struct CalcResult {
    pub value: String,
}

pub fn search(query: &str) -> Option<CalcResult> {
    // Optimization: If there are no digits, it's not math.
    if !query.chars().any(|c| c.is_ascii_digit()) {
        return None;
    }

    // evalexpr handles order of operations, functions (sin, cos), etc.
    match eval(query) {
        Ok(val) => {
            let result_str = val.to_string();
            // If the result is identical to input (e.g. "5"), ignore it.
            if result_str == query { return None; }

            Some(CalcResult { value: result_str })
        },
        Err(_) => None,
    }
}