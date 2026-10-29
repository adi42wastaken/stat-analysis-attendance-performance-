## This C program analyzes the relationship between student attendance and academic marks using data from a CSV file. It can analyze mid-semester marks, quiz marks, or both.


### How to run
- Clone the repo(just one code and .csv file)
- Replace "attendance" by real attendance of students(out of 100)
- "midsem" by real marks(out of 100)
- "quiz" by average of all the quizzes (out of 100)
- make sure code can read the path of .csv file and run the program.
- Note the student_id don't really affect our data/code so, it won't be necessary to change that, although we can if we want


**How it works**

Loads student data: Reads the CSV file, separates rows into fields, converts scores to numbers, and skips invalid records. Attendance and marks are expected to be percentages from 0 to 100.
Lets the user choose marks: The menu offers mid-semester, quiz, or both, and then provides individual analysis options or a “run everything” option.
Calculates statistics: Reports mean, median, standard deviation, minimum, and maximum.
Studies relationships: Calculates Pearson and Spearman correlations, tests statistical significance with p-values, fits a linear regression, and makes example predictions.
Compares attendance groups: Uses a 75% attendance cut-off to compare marks for students above and below it with a Welch t-test.
Generates output: Displays results on screen and can save a text report, scatter plot, histogram, and correlation heatmap. The graphs are written as SVG files that can be opened in a web browser.

**Overall purpose**

The program helps explore whether attendance and marks tend to vary together in the dataset. Its results describe relationships, not proof that attendance causes higher or lower marks; other factors may also affect performance.
