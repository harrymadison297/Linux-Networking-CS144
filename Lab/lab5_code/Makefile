TAR = lab5_submit.tar.gz
SUBMISSION_SITE = https://web.stanford.edu/class/cs144/cgi-bin/submit/
SUBMIT = *.[ch] ctcp/*.[ch] nat/router/*.[ch] README

simple_curl: simple_curl.c
	gcc -o simple_curl simple_curl.c

all: simple_curl

clean:
	rm -f *.o simple_curl

submit: clean
	tar -czf $(TAR) $(SUBMIT)
	@echo
	@echo
	@echo '  Your submission file $(TAR) was successfully created.'
	@echo '  Please submit it to the following URL: '
	@echo '   $(SUBMISSION_SITE)'
	@echo

.PHONY: clean