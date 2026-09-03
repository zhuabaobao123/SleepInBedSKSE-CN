#pragma once

class ScopedFlag
{
public:
	explicit ScopedFlag(bool& flag) :
		_flag(flag), _previous(flag)
	{
		_flag = true;
	}

	~ScopedFlag()
	{
		_flag = _previous;
	}

	ScopedFlag(const ScopedFlag&) = delete;
	ScopedFlag& operator=(const ScopedFlag&) = delete;

private:
	bool& _flag;
	bool  _previous;
};
