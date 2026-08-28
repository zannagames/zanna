10 ' Example 1: categorize die rolls
20 FOR ROLL = 1 TO 6
30   SELECT CASE ROLL
40     CASE 1
50       PRINT "Rolled a one"
60     CASE 2, 3, 4
70       PRINT "Mid-range roll "; ROLL
80     CASE 5, 6
90       PRINT "High roll "; ROLL
100    CASE ELSE
110      PRINT "Unexpected value"
120  END SELECT
130 NEXT ROLL
140 PRINT
150 PRINT "Example 2: weekday names"
160 FOR DAY = 1 TO 8
170   SELECT CASE DAY
180     CASE 1
190       PRINT "Monday"
200     CASE 2
210       PRINT "Tuesday"
220     CASE 3
230       PRINT "Wednesday"
240     CASE 4
250       PRINT "Thursday"
260     CASE 5
270       PRINT "Friday"
280     CASE 6
290       PRINT "Saturday"
300     CASE 7
310       PRINT "Sunday"
320     CASE ELSE
330       PRINT "Invalid day"
340   END SELECT
350 NEXT DAY
360 END
