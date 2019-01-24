package com.theeconomist.downloader.bean;

public class TimeBean {

    /**
     * now playing times
     */
    private int currSecs;

    /**
     * total times(only can get duration)
     */
    private int totalSecs;

    public int getCurrSecs() {
        return currSecs;
    }

    public void setCurrSecs(int currSecs) {
        this.currSecs = currSecs;
    }

    public int getTotalSecs() {
        return totalSecs;
    }

    public void setTotalSecs(int totalSecs) {
        this.totalSecs = totalSecs;
    }

    @Override
    public String toString() {
        return "TimeBean{" +
                "currSecs=" + currSecs +
                ", totalSecs=" + totalSecs +
                '}';
    }
}
