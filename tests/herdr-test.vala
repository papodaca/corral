int main () {
    var path = Environment.find_program_in_path ("herdr");
    if (path != null) {
        assert (path.length > 0);
        assert (FileUtils.test (path, FileTest.IS_EXECUTABLE));
    }
    return 0;
}
